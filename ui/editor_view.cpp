/* HaikuIDE text view.
 * The view stays the size of the viewport. Draw paints lines that intersect
 * the clip. A full-document framebuffer blit is too slow on this guest.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#include "editor_view.hpp"

#include <Clipboard.h>
#include <Font.h>
#include <ScrollBar.h>
#include <Window.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace haikuide {
namespace {

size_t ContentEnd(const Document& doc, size_t line) {
    if (line + 1 < doc.line_count()) {
        const size_t next = doc.line_start(line + 1);
        return next == 0 ? 0 : next - 1;
    }
    return doc.size();
}

size_t Utf8Len(unsigned char lead) {
    if (lead < 0x80) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

size_t NextByte(const std::string& text, size_t i) {
    if (i >= text.size()) {
        return text.size();
    }
    size_t n = Utf8Len(static_cast<unsigned char>(text[i]));
    if (i + n > text.size()) {
        n = 1;
    }
    return i + n;
}

size_t PrevByte(const std::string& text, size_t i) {
    if (i == 0) {
        return 0;
    }
    --i;
    while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
        --i;
    }
    return i;
}

bool IsWord(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n'; }

size_t WordLeft(const std::string& text, size_t i) {
    if (i == 0) {
        return 0;
    }
    i = PrevByte(text, i);
    while (i > 0 && IsSpace(text[i])) {
        i = PrevByte(text, i);
    }
    while (i > 0) {
        const size_t prev = PrevByte(text, i);
        if (IsSpace(text[prev]) || !IsWord(static_cast<unsigned char>(text[prev]))) {
            break;
        }
        i = prev;
    }
    return i;
}

size_t WordRight(const std::string& text, size_t i) {
    const size_t n = text.size();
    while (i < n && !IsSpace(text[i]) && IsWord(static_cast<unsigned char>(text[i]))) {
        i = NextByte(text, i);
    }
    while (i < n && IsSpace(text[i])) {
        i = NextByte(text, i);
    }
    return i;
}

bool RangeIsSimple(const std::string& text, size_t begin, size_t end) {
    for (size_t i = begin; i < end && i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\t' || c >= 0x80) {
            return false;
        }
    }
    return true;
}

BFont PickMonoFont() {
    const char* families[][2] = {
        {"Noto Sans Mono", "Regular"},
        {"Noto Sans Mono", "Book"},
        {"DejaVu Sans Mono", "Book"},
        {"DejaVu Sans Mono", "Regular"},
        {"Noto Mono", "Regular"},
    };
    BFont font;
    for (const auto& pair : families) {
        if (font.SetFamilyAndStyle(pair[0], pair[1]) == B_OK) {
            font.SetSize(13.f);
            return font;
        }
    }
    font = *be_fixed_font;
    font.SetSize(13.f);
    return font;
}

std::string NormalizeNewlines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace

GutterView::GutterView(BRect frame)
    : BView(frame, "gutter", B_FOLLOW_NONE, B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE) {}

void GutterView::SetEditor(EditorView* editor) { editor_ = editor; }

void GutterView::AttachedToWindow() {
    BView::AttachedToWindow();
    SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));
}

void GutterView::Draw(BRect update) {
    if (editor_ == nullptr) {
        SetHighColor(ui_color(B_PANEL_BACKGROUND_COLOR));
        FillRect(update);
        return;
    }
    const rgb_color bg = editor_->GutterColor();
    SetHighColor(bg);
    FillRect(update);
    const EditorTab* tab = editor_->Tab();
    if (tab == nullptr) {
        return;
    }
    BFont font;
    editor_->GetFont(&font);
    SetFont(&font);
    const float lh = editor_->LineHeight();
    if (lh < 1.f) {
        return;
    }
    int first = static_cast<int>(std::floor(update.top / lh));
    int last = static_cast<int>(std::floor(update.bottom / lh));
    if (first < 0) {
        first = 0;
    }
    const int count = static_cast<int>(tab->doc.line_count());
    if (last >= count) {
        last = count - 1;
    }
    for (int line = first; line <= last; ++line) {
        int worst = 0;
        for (const LineDiag& diag : tab->diags) {
            if (diag.line != line) {
                continue;
            }
            const int severity = diag.severity == 0 ? 3 : diag.severity;
            if (worst == 0 || severity < worst) {
                worst = severity;
            }
        }
        const float y = static_cast<float>(line) * lh + editor_->Ascent();
        if (worst != 0) {
            rgb_color mark{40, 90, 160, 255};
            if (worst == 1) {
                mark = rgb_color{190, 40, 40, 255};
            } else if (worst == 2) {
                mark = rgb_color{190, 150, 30, 255};
            }
            SetHighColor(mark);
            const float mid = static_cast<float>(line) * lh + lh * 0.5f;
            FillRect(BRect(3.f, mid - 3.f, 9.f, mid + 3.f));
        }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%d", line + 1);
        const float w = StringWidth(buf);
        SetHighColor(editor_->NumberColor());
        DrawString(buf, BPoint(Bounds().Width() - w - 8.f, y));
    }
    SetHighColor(editor_->NumberColor());
    StrokeLine(BPoint(Bounds().right, update.top), BPoint(Bounds().right, update.bottom));
}

void GutterView::MouseDown(BPoint where) {
    if (editor_ == nullptr || editor_->LineHeight() < 1.f) {
        return;
    }
    editor_->MakeFocus(true);
    int line = static_cast<int>(std::floor(where.y / editor_->LineHeight()));
    if (line < 0) {
        line = 0;
    }
    const bool shift = (modifiers() & B_SHIFT_KEY) != 0;
    editor_->SelectLine(static_cast<size_t>(line), shift);
}

EditorView::EditorView(BRect frame)
    : BView(frame, "editor", B_FOLLOW_NONE,
            B_WILL_DRAW | B_NAVIGABLE | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE) {
    ApplyFont();
}

void EditorView::SetHost(EditorHost* host) { host_ = host; }

void EditorView::SetGutter(GutterView* gutter) {
    gutter_ = gutter;
    SyncGutter();
}

void EditorView::SetTab(EditorTab* tab) {
    if (tab_ != nullptr && tab_ != tab) {
        tab_->scroll_x = Bounds().left;
        tab_->scroll_y = Bounds().top;
    }
    tab_ = tab;
    dragging_ = false;
    if (tab_ != nullptr) {
        if (tab_->flat.size() != tab_->doc.size()) {
            tab_->flat = tab_->doc.text();
        }
        RecomputeWidth();
        ScrollTo(BPoint(tab_->scroll_x, tab_->scroll_y));
    }
    Invalidate();
    if (gutter_ != nullptr) {
        gutter_->Invalidate();
    }
    if (host_ != nullptr) {
        host_->OnEditorChanged();
    }
}

float EditorView::ContentHeight() const {
    size_t lines = tab_ != nullptr ? tab_->doc.line_count() : 1;
    if (lines < 1) {
        lines = 1;
    }
    return static_cast<float>(lines) * line_h_;
}

float EditorView::ContentWidth() const {
    if (tab_ == nullptr) {
        return cell_;
    }
    return std::max(tab_->content_w, cell_);
}

float EditorView::GutterWidth() const {
    if (!line_numbers_) {
        return 0.f;
    }
    size_t lines = tab_ != nullptr ? tab_->doc.line_count() : 1;
    int digits = 1;
    while (lines >= 10) {
        ++digits;
        lines /= 10;
    }
    if (digits < 2) {
        digits = 2;
    }
    return static_cast<float>(digits) * cell_ + 16.f;
}

void EditorView::SetLineNumbers(bool on) {
    if (line_numbers_ == on) {
        return;
    }
    line_numbers_ = on;
    if (host_ != nullptr) {
        host_->OnEditorChanged();
    }
}

void EditorView::ApplyFont() {
    const BFont font = PickMonoFont();
    SetFont(&font);
    font_height height;
    GetFontHeight(&height);
    ascent_ = height.ascent;
    line_h_ = std::ceil(height.ascent + height.descent + 1.f);
    if (line_h_ < 1.f) {
        line_h_ = 1.f;
    }
    cell_ = StringWidth("M");
    if (cell_ < 1.f) {
        cell_ = 8.f;
    }
    LoadColors();
    SyncGutter();
    font_family family;
    font_style style;
    font.GetFamilyAndStyle(&family, &style);
    if (!font_ready_) {
        std::fprintf(stderr, "HaikuIDE font: %s %s cell %.2f line %.1f\n", family, style,
                     cell_, line_h_);
    }
    font_ready_ = true;
}

void EditorView::LoadColors() {
    bg_ = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
    fg_ = ui_color(B_DOCUMENT_TEXT_COLOR);
    bg_.alpha = 255;
    fg_.alpha = 255;
    if (bg_.IsDark()) {
        sel_.set_to(36, 72, 120, 255);
    } else {
        sel_.set_to(173, 214, 255, 255);
    }
    gutter_bg_.set_to(static_cast<uint8>((bg_.red * 8 + 180) / 10),
                      static_cast<uint8>((bg_.green * 8 + 180) / 10),
                      static_cast<uint8>((bg_.blue * 8 + 180) / 10), 255);
    num_fg_.set_to(static_cast<uint8>((fg_.red + bg_.red) / 2),
                   static_cast<uint8>((fg_.green + bg_.green) / 2),
                   static_cast<uint8>((fg_.blue + bg_.blue) / 2), 255);
    SetViewColor(bg_);
    SetLowColor(bg_);
}

void EditorView::SyncGutter() {
    if (gutter_ == nullptr) {
        return;
    }
    BFont font;
    GetFont(&font);
    gutter_->SetFont(&font);
    gutter_->SetViewColor(gutter_bg_);
}

void EditorView::AttachedToWindow() {
    BView::AttachedToWindow();
    ApplyFont();
    MakeFocus(true);
}

void EditorView::FrameResized(float /*newWidth*/, float /*newHeight*/) {
    if (host_ != nullptr) {
        host_->OnViewportResized();
    }
}

void EditorView::ScrollTo(BPoint where) {
    if (where.x < 0.f) {
        where.x = 0.f;
    }
    if (where.y < 0.f) {
        where.y = 0.f;
    }
    const BRect bounds = Bounds();
    const bool same = std::fabs(bounds.left - where.x) < 0.5f &&
                      std::fabs(bounds.top - where.y) < 0.5f;
    if (!same) {
        BView::ScrollTo(where);
        if (tab_ != nullptr) {
            tab_->scroll_x = where.x;
            tab_->scroll_y = where.y;
        }
    }
    if (gutter_ != nullptr && std::fabs(gutter_->Bounds().top - where.y) >= 0.5f) {
        gutter_->ScrollTo(BPoint(0.f, where.y));
    }
    if (same || scroll_lock_) {
        return;
    }
    scroll_lock_ = true;
    if (BScrollBar* bar = ScrollBar(B_VERTICAL)) {
        if (std::fabs(bar->Value() - where.y) >= 0.5f) {
            bar->SetValue(where.y);
        }
    }
    if (BScrollBar* bar = ScrollBar(B_HORIZONTAL)) {
        if (std::fabs(bar->Value() - where.x) >= 0.5f) {
            bar->SetValue(where.x);
        }
    }
    scroll_lock_ = false;
}

void EditorView::RecomputeWidth() {
    if (tab_ == nullptr) {
        return;
    }
    float max_w = 0.f;
    const size_t lines = tab_->doc.line_count();
    for (size_t line = 0; line < lines; ++line) {
        size_t begin = tab_->doc.line_start(line);
        size_t end = ContentEnd(tab_->doc, line);
        if (begin > tab_->flat.size()) {
            begin = tab_->flat.size();
        }
        if (end > tab_->flat.size()) {
            end = tab_->flat.size();
        }
        max_w = std::max(max_w, WidthOf(begin, end));
    }
    tab_->content_w = max_w + cell_;
}

void EditorView::ClampCaret() {
    if (tab_ == nullptr) {
        return;
    }
    const size_t n = tab_->doc.size();
    if (tab_->caret > n) {
        tab_->caret = n;
    }
    if (tab_->anchor > n) {
        tab_->anchor = n;
    }
}

void EditorView::FinishRefresh(bool remember_x) {
    if (tab_ == nullptr) {
        return;
    }
    tab_->flat = tab_->doc.text();
    tab_->dirty = tab_->flat != tab_->saved;
    ClampCaret();
    RecomputeWidth();
    if (remember_x) {
        tab_->preferred_x = CaretX();
    }
    RevealCaret();
    Invalidate();
    if (gutter_ != nullptr) {
        gutter_->Invalidate();
    }
    if (host_ != nullptr) {
        host_->OnEditorChanged();
    }
}

bool EditorView::ReplaceRange(size_t pos, size_t len, std::string_view text) {
    if (tab_ == nullptr) {
        return false;
    }
    if (pos > tab_->doc.size() || len > tab_->doc.size() - pos) {
        return false;
    }
    if (len == 0 && text.empty()) {
        return true;
    }
    tab_->undo_marks.push_back(CaretMark{tab_->caret, tab_->anchor});
    tab_->redo_marks.clear();
    bool ok = false;
    if (len == 0) {
        ok = tab_->doc.insert(pos, text);
    } else if (text.empty()) {
        ok = tab_->doc.erase(pos, len);
    } else {
        ok = tab_->doc.replace(pos, len, text);
    }
    if (!ok) {
        tab_->undo_marks.pop_back();
        return false;
    }
    tab_->caret = pos + text.size();
    tab_->anchor = tab_->caret;
    if (host_ != nullptr) {
        host_->OnEdit(pos, len, std::string(text));
    }
    FinishRefresh(true);
    return true;
}

void EditorView::InsertText(std::string_view text) {
    if (tab_ == nullptr || text.empty()) {
        return;
    }
    const size_t a = std::min(tab_->caret, tab_->anchor);
    const size_t b = std::max(tab_->caret, tab_->anchor);
    ReplaceRange(a, b - a, text);
}

void EditorView::ApplyEdit(size_t pos, size_t len, const std::string& text) {
    ReplaceRange(pos, len, text);
}

void EditorView::PlaceCaret(size_t byte) {
    if (tab_ == nullptr) {
        return;
    }
    if (byte > tab_->doc.size()) {
        byte = tab_->doc.size();
    }
    tab_->caret = byte;
    tab_->anchor = byte;
    tab_->preferred_x = CaretX();
    RevealCaret();
    Invalidate();
    if (gutter_ != nullptr) {
        gutter_->Invalidate();
    }
}

void EditorView::Undo() {
    if (tab_ == nullptr || !tab_->doc.can_undo()) {
        return;
    }
    tab_->redo_marks.push_back(CaretMark{tab_->caret, tab_->anchor});
    if (!tab_->doc.undo()) {
        tab_->redo_marks.pop_back();
        return;
    }
    if (!tab_->undo_marks.empty()) {
        const CaretMark mark = tab_->undo_marks.back();
        tab_->undo_marks.pop_back();
        tab_->caret = mark.caret;
        tab_->anchor = mark.anchor;
    }
    FinishRefresh(true);
    if (host_ != nullptr) {
        host_->OnFullSync();
    }
}

void EditorView::Redo() {
    if (tab_ == nullptr || !tab_->doc.can_redo()) {
        return;
    }
    tab_->undo_marks.push_back(CaretMark{tab_->caret, tab_->anchor});
    if (!tab_->doc.redo()) {
        tab_->undo_marks.pop_back();
        return;
    }
    if (!tab_->redo_marks.empty()) {
        const CaretMark mark = tab_->redo_marks.back();
        tab_->redo_marks.pop_back();
        tab_->caret = mark.caret;
        tab_->anchor = mark.anchor;
    }
    FinishRefresh(true);
    if (host_ != nullptr) {
        host_->OnFullSync();
    }
}

void EditorView::Copy() {
    if (tab_ == nullptr || tab_->caret == tab_->anchor) {
        return;
    }
    const size_t a = std::min(tab_->caret, tab_->anchor);
    const size_t b = std::max(tab_->caret, tab_->anchor);
    if (b > tab_->flat.size() || a > b) {
        return;
    }
    const std::string piece = tab_->flat.substr(a, b - a);
    if (!be_clipboard->Lock()) {
        return;
    }
    be_clipboard->Clear();
    if (BMessage* data = be_clipboard->Data()) {
        data->AddData("text/plain", B_MIME_TYPE, piece.data(), piece.size());
        be_clipboard->Commit();
    }
    be_clipboard->Unlock();
}

void EditorView::Cut() {
    if (tab_ == nullptr || tab_->caret == tab_->anchor) {
        return;
    }
    Copy();
    const size_t a = std::min(tab_->caret, tab_->anchor);
    const size_t b = std::max(tab_->caret, tab_->anchor);
    ReplaceRange(a, b - a, "");
}

void EditorView::Paste() {
    if (tab_ == nullptr || !be_clipboard->Lock()) {
        return;
    }
    const void* bytes = nullptr;
    ssize_t length = 0;
    BMessage* data = be_clipboard->Data();
    status_t found = B_ERROR;
    if (data != nullptr) {
        found = data->FindData("text/plain", B_MIME_TYPE, &bytes, &length);
    }
    std::string text;
    if (found == B_OK && bytes != nullptr && length > 0) {
        text.assign(static_cast<const char*>(bytes), static_cast<size_t>(length));
    }
    be_clipboard->Unlock();
    if (text.empty()) {
        return;
    }
    InsertText(NormalizeNewlines(text));
}

void EditorView::SelectAll() {
    if (tab_ == nullptr) {
        return;
    }
    tab_->anchor = 0;
    tab_->caret = tab_->doc.size();
    tab_->preferred_x = CaretX();
    RevealCaret();
    Invalidate();
}

void EditorView::SelectLine(size_t line, bool extend) {
    if (tab_ == nullptr || tab_->doc.line_count() == 0) {
        return;
    }
    if (line >= tab_->doc.line_count()) {
        line = tab_->doc.line_count() - 1;
    }
    const size_t start = tab_->doc.line_start(line);
    const size_t end = (line + 1 < tab_->doc.line_count()) ? tab_->doc.line_start(line + 1)
                                                           : tab_->doc.size();
    if (!extend) {
        tab_->anchor = start;
    }
    tab_->caret = end;
    tab_->preferred_x = CaretX();
    RevealCaret();
    Invalidate();
}

void EditorView::DeleteWord(bool forward) {
    if (tab_ == nullptr) {
        return;
    }
    if (tab_->caret != tab_->anchor) {
        const size_t a = std::min(tab_->caret, tab_->anchor);
        const size_t b = std::max(tab_->caret, tab_->anchor);
        ReplaceRange(a, b - a, "");
        return;
    }
    const size_t from = tab_->caret;
    size_t to = forward ? WordRight(tab_->flat, from) : WordLeft(tab_->flat, from);
    if (from == to) {
        return;
    }
    if (to < from) {
        ReplaceRange(to, from - to, "");
    } else {
        ReplaceRange(from, to - from, "");
    }
}

size_t EditorView::LineOf(size_t byte) const {
    if (tab_ == nullptr || tab_->doc.line_count() == 0) {
        return 0;
    }
    size_t lo = 0;
    size_t hi = tab_->doc.line_count();
    while (lo + 1 < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (tab_->doc.line_start(mid) <= byte) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

float EditorView::WidthOf(size_t begin, size_t end) const {
    if (tab_ == nullptr || end <= begin) {
        return 0.f;
    }
    if (begin > tab_->flat.size()) {
        return 0.f;
    }
    if (end > tab_->flat.size()) {
        end = tab_->flat.size();
    }
    if (RangeIsSimple(tab_->flat, begin, end)) {
        return static_cast<float>(end - begin) * cell_;
    }
    float x = 0.f;
    const float stop = std::max(1.f, cell_ * 4.f);
    for (size_t i = begin; i < end;) {
        const unsigned char c = static_cast<unsigned char>(tab_->flat[i]);
        if (c == '\t') {
            x = (std::floor(x / stop) + 1.f) * stop;
            ++i;
            continue;
        }
        size_t n = Utf8Len(c);
        if (i + n > end) {
            n = 1;
        }
        char tmp[8];
        if (n >= sizeof(tmp)) {
            n = 1;
        }
        std::memcpy(tmp, tab_->flat.data() + i, n);
        tmp[n] = '\0';
        x += StringWidth(tmp);
        i += n;
    }
    return x;
}

float EditorView::CaretX() const {
    if (tab_ == nullptr) {
        return 0.f;
    }
    const size_t line = LineOf(tab_->caret);
    const size_t start = tab_->doc.line_start(line);
    size_t end = ContentEnd(tab_->doc, line);
    size_t caret = tab_->caret;
    if (caret < start) {
        caret = start;
    }
    if (caret > end) {
        caret = end;
    }
    return WidthOf(start, caret);
}

BPoint EditorView::CaretScreenPoint() const {
    const size_t caret = tab_ != nullptr ? tab_->caret : 0;
    const float y = static_cast<float>(LineOf(caret) + 1) * line_h_;
    return ConvertToScreen(BPoint(CaretX(), y));
}

size_t EditorView::OffsetOnLine(size_t line, float x) const {
    if (tab_ == nullptr) {
        return 0;
    }
    const size_t start = tab_->doc.line_start(line);
    size_t end = ContentEnd(tab_->doc, line);
    if (start > tab_->flat.size()) {
        return tab_->flat.size();
    }
    if (end > tab_->flat.size()) {
        end = tab_->flat.size();
    }
    if (x <= 0.f) {
        return start;
    }
    if (RangeIsSimple(tab_->flat, start, end) && cell_ > 0.f) {
        size_t cols = static_cast<size_t>(std::floor((x / cell_) + 0.5f));
        if (cols > end - start) {
            cols = end - start;
        }
        return start + cols;
    }
    float at = 0.f;
    size_t i = start;
    while (i < end) {
        size_t n = NextByte(tab_->flat, i);
        if (n > end) {
            n = end;
        }
        const float w = WidthOf(i, n);
        if (x < at + w * 0.5f) {
            return i;
        }
        at += w;
        i = n;
    }
    return end;
}

size_t EditorView::OffsetAt(BPoint where) const {
    if (tab_ == nullptr || tab_->doc.line_count() == 0 || line_h_ < 1.f) {
        return 0;
    }
    int line = static_cast<int>(std::floor(where.y / line_h_));
    if (line < 0) {
        line = 0;
    }
    if (static_cast<size_t>(line) >= tab_->doc.line_count()) {
        line = static_cast<int>(tab_->doc.line_count()) - 1;
    }
    return OffsetOnLine(static_cast<size_t>(line), where.x);
}

void EditorView::RevealCaret() {
    if (tab_ == nullptr || line_h_ < 1.f) {
        return;
    }
    const size_t line = LineOf(tab_->caret);
    const float y = static_cast<float>(line) * line_h_;
    const float x = CaretX();
    const BRect bounds = Bounds();
    float next_y = bounds.top;
    float next_x = bounds.left;
    if (y < bounds.top) {
        next_y = y;
    } else if (y + line_h_ > bounds.bottom) {
        next_y = y + line_h_ - bounds.Height();
    }
    if (x < bounds.left + 2.f) {
        next_x = std::max(0.f, x - 2.f);
    } else if (x + 2.f > bounds.right) {
        next_x = x + 2.f - bounds.Width();
    }
    if (next_y < 0.f) {
        next_y = 0.f;
    }
    if (next_x < 0.f) {
        next_x = 0.f;
    }
    const float max_y = std::max(0.f, ContentHeight() - bounds.Height());
    const float max_x = std::max(0.f, ContentWidth() - bounds.Width());
    if (next_y > max_y) {
        next_y = max_y;
    }
    if (next_x > max_x) {
        next_x = max_x;
    }
    if (std::fabs(next_y - bounds.top) >= 0.5f || std::fabs(next_x - bounds.left) >= 0.5f) {
        ScrollTo(BPoint(next_x, next_y));
    }
}

void EditorView::MoveCaret(size_t next, bool shift, bool remember_x) {
    if (tab_ == nullptr) {
        return;
    }
    if (next > tab_->doc.size()) {
        next = tab_->doc.size();
    }
    tab_->caret = next;
    if (!shift) {
        tab_->anchor = next;
    }
    if (remember_x) {
        tab_->preferred_x = CaretX();
    }
    RevealCaret();
    Invalidate();
}

void EditorView::MoveVertical(int delta, bool shift) {
    if (tab_ == nullptr || tab_->doc.line_count() == 0) {
        return;
    }
    size_t line = LineOf(tab_->caret);
    if (delta < 0) {
        const size_t step = static_cast<size_t>(-delta);
        line = step >= line ? 0 : line - step;
    } else if (line + static_cast<size_t>(delta) >= tab_->doc.line_count()) {
        line = tab_->doc.line_count() - 1;
    } else {
        line += static_cast<size_t>(delta);
    }
    MoveCaret(OffsetOnLine(line, tab_->preferred_x), shift, false);
}

void EditorView::DrawLineText(size_t begin, size_t end, BRect update, float baseline) {
    if (tab_ == nullptr || end <= begin) {
        return;
    }
    if (begin > tab_->flat.size()) {
        return;
    }
    if (end > tab_->flat.size()) {
        end = tab_->flat.size();
    }
    if (RangeIsSimple(tab_->flat, begin, end) && cell_ > 0.f) {
        size_t col0 = 0;
        size_t col1 = end - begin;
        if (update.left > 0.f) {
            const double raw = std::floor(update.left / cell_) - 1.0;
            if (raw > 0.0) {
                col0 = static_cast<size_t>(raw);
            }
        }
        const double raw_end = std::floor(update.right / cell_) + 2.0;
        if (raw_end < static_cast<double>(col1)) {
            col1 = static_cast<size_t>(std::max(0.0, raw_end));
        }
        if (col0 > end - begin) {
            return;
        }
        if (col1 < col0) {
            col1 = col0;
        }
        if (col1 > end - begin) {
            col1 = end - begin;
        }
        if (col1 == col0) {
            return;
        }
        const std::string piece = tab_->flat.substr(begin + col0, col1 - col0);
        DrawString(piece.c_str(), BPoint(static_cast<float>(col0) * cell_, baseline));
        return;
    }
    const std::string piece = tab_->flat.substr(begin, end - begin);
    DrawString(piece.c_str(), BPoint(0.f, baseline));
}

void EditorView::Draw(BRect update) {
    if (line_h_ < 1.f || tab_ == nullptr) {
        SetHighColor(bg_);
        FillRect(update);
        return;
    }
    const size_t lines = tab_->doc.line_count();
    int first = static_cast<int>(std::floor(update.top / line_h_));
    int last = static_cast<int>(std::floor(update.bottom / line_h_));
    if (first < 0) {
        first = 0;
    }
    const size_t sel_a = std::min(tab_->caret, tab_->anchor);
    const size_t sel_b = std::max(tab_->caret, tab_->anchor);
    const bool show_caret = Window() != nullptr && Window()->IsActive() && IsFocus();
    const size_t caret_line = LineOf(tab_->caret);

    for (int line = first; line <= last && static_cast<size_t>(line) < lines; ++line) {
        const float y = static_cast<float>(line) * line_h_;
        const BRect row(update.left, y, update.right, y + line_h_ - 1.f);
        SetDrawingMode(B_OP_COPY);
        SetHighColor(bg_);
        FillRect(row);
        const size_t start = tab_->doc.line_start(static_cast<size_t>(line));
        size_t end = ContentEnd(tab_->doc, static_cast<size_t>(line));
        if (end > tab_->flat.size()) {
            end = tab_->flat.size();
        }
        if (sel_a != sel_b && sel_b > start && sel_a <= end) {
            const size_t from = std::min(std::max(sel_a, start), end);
            const size_t to = std::min(std::max(sel_b, start), end);
            float x0 = WidthOf(start, from);
            float x1 = WidthOf(start, to);
            if (sel_b > end) {
                x1 = std::max(x1, update.right);
            }
            if (x1 > x0) {
                SetHighColor(sel_);
                FillRect(BRect(x0, y, x1, y + line_h_ - 1.f));
            }
        }
        SetDrawingMode(B_OP_OVER);
        SetHighColor(fg_);
        DrawLineText(start, end, update, y + ascent_);
        for (const LineDiag& diag : tab_->diags) {
            if (diag.end_line < line || diag.line > line) {
                continue;
            }
            size_t from = start;
            size_t to = end;
            if (diag.line == line) {
                from = diag.start_byte;
            }
            if (diag.end_line == line) {
                to = diag.end_byte;
            }
            if (from < start) {
                from = start;
            }
            if (to > end) {
                to = end;
            }
            if (to < from) {
                to = from;
            }
            float x0 = WidthOf(start, from);
            float x1 = WidthOf(start, to);
            if (x1 < x0 + 2.f) {
                x1 = x0 + cell_;
            }
            rgb_color wave{190, 40, 40, 255};
            if (diag.severity == 2) {
                wave = rgb_color{180, 140, 20, 255};
            } else if (diag.severity > 2) {
                wave = rgb_color{40, 90, 160, 255};
            }
            SetHighColor(wave);
            const float base = y + line_h_ - 3.f;
            float x = x0;
            bool up = false;
            while (x < x1) {
                const float next = std::min(x1, x + 3.f);
                StrokeLine(BPoint(x, base + (up ? -2.f : 0.f)),
                           BPoint(next, base + (up ? 0.f : -2.f)));
                x = next;
                up = !up;
            }
        }
        if (show_caret && static_cast<size_t>(line) == caret_line) {
            size_t caret = tab_->caret;
            if (caret < start) {
                caret = start;
            }
            if (caret > end) {
                caret = end;
            }
            const float x = WidthOf(start, caret);
            SetDrawingMode(B_OP_COPY);
            SetHighColor(fg_);
            FillRect(BRect(x, y + 1.f, x + 1.f, y + line_h_ - 2.f));
        }
    }
    const float bottom = static_cast<float>(lines) * line_h_;
    if (update.bottom > bottom) {
        SetDrawingMode(B_OP_COPY);
        SetHighColor(bg_);
        FillRect(BRect(update.left, std::max(update.top, bottom), update.right, update.bottom));
    }
}

void EditorView::KeyDown(const char* bytes, int32 numBytes) {
    if (tab_ == nullptr || bytes == nullptr || numBytes < 1) {
        return;
    }
    const uint32 mods = modifiers();
    const bool ctrl = (mods & B_CONTROL_KEY) != 0;
    const bool shift = (mods & B_SHIFT_KEY) != 0;
    const bool cmd = (mods & B_COMMAND_KEY) != 0;
    int32 raw = static_cast<unsigned char>(bytes[0]);
    if (Window() != nullptr && Window()->CurrentMessage() != nullptr) {
        int32 value = 0;
        if (Window()->CurrentMessage()->FindInt32("raw_char", &value) == B_OK) {
            raw = value;
        }
    }
    int letter = 0;
    if (raw >= 'a' && raw <= 'z') {
        letter = raw - 'a' + 'A';
    } else if (raw >= 'A' && raw <= 'Z') {
        letter = raw;
    }

    if (host_ != nullptr && host_->OnCompletionKey(raw)) {
        return;
    }
    if (host_ != nullptr && Window() != nullptr && Window()->CurrentMessage() != nullptr) {
        int32 key = 0;
        const bool function = static_cast<unsigned char>(bytes[0]) == B_FUNCTION_KEY ||
                              raw == B_FUNCTION_KEY;
        if (function && Window()->CurrentMessage()->FindInt32("key", &key) == B_OK &&
            key == B_F12_KEY) {
            host_->OnGoToDefinition();
            return;
        }
    }

    if (ctrl && !cmd) {
        if (raw == B_SPACE || raw == ' ') {
            if (host_ != nullptr) {
                host_->OnComplete(1, ' ');
            }
            return;
        }
        if (raw == B_LEFT_ARROW) {
            MoveCaret(WordLeft(tab_->flat, tab_->caret), shift, true);
            return;
        }
        if (raw == B_RIGHT_ARROW) {
            MoveCaret(WordRight(tab_->flat, tab_->caret), shift, true);
            return;
        }
        if (raw == B_HOME) {
            MoveCaret(0, shift, true);
            return;
        }
        if (raw == B_END) {
            MoveCaret(tab_->doc.size(), shift, true);
            return;
        }
        if (raw == B_BACKSPACE) {
            DeleteWord(false);
            return;
        }
        if (raw == B_DELETE) {
            DeleteWord(true);
            return;
        }
        if (letter != 0 && host_ != nullptr) {
            switch (letter) {
            case 'A':
                if (!shift) {
                    SelectAll();
                }
                return;
            case 'Z':
                if (shift) {
                    Redo();
                } else {
                    Undo();
                }
                return;
            case 'S':
                host_->Command(shift ? kMsgSaveAs : kMsgSave);
                return;
            case 'X':
                Cut();
                return;
            case 'C':
                Copy();
                return;
            case 'V':
                Paste();
                return;
            case 'N':
                host_->Command(kMsgNew);
                return;
            case 'O':
                host_->Command(kMsgOpen);
                return;
            case 'W':
                host_->Command(kMsgClose);
                return;
            case 'Q':
                host_->Command(kMsgQuit);
                return;
            case 'L':
                host_->Command(kMsgLineNumbers);
                return;
            default:
                return;
            }
        }
        if (letter != 0) {
            return;
        }
    }

    const unsigned char ch = static_cast<unsigned char>(bytes[0]);
    if (!ctrl && !cmd && !shift && tab_->caret != tab_->anchor &&
        (ch == B_LEFT_ARROW || ch == B_RIGHT_ARROW)) {
        const size_t lo = std::min(tab_->caret, tab_->anchor);
        const size_t hi = std::max(tab_->caret, tab_->anchor);
        MoveCaret(ch == B_LEFT_ARROW ? lo : hi, false, true);
        return;
    }

    switch (ch) {
    case B_LEFT_ARROW: {
        size_t next = tab_->caret;
        if (!shift && tab_->caret != tab_->anchor) {
            next = std::min(tab_->caret, tab_->anchor);
        } else {
            next = PrevByte(tab_->flat, tab_->caret);
        }
        MoveCaret(next, shift, true);
        return;
    }
    case B_RIGHT_ARROW: {
        size_t next = tab_->caret;
        if (!shift && tab_->caret != tab_->anchor) {
            next = std::max(tab_->caret, tab_->anchor);
        } else {
            next = NextByte(tab_->flat, tab_->caret);
        }
        MoveCaret(next, shift, true);
        return;
    }
    case B_UP_ARROW:
        MoveVertical(-1, shift);
        return;
    case B_DOWN_ARROW:
        MoveVertical(1, shift);
        return;
    case B_PAGE_UP:
    case B_PAGE_DOWN: {
        int page = static_cast<int>(Bounds().Height() / line_h_) - 1;
        if (page < 1) {
            page = 1;
        }
        MoveVertical(ch == B_PAGE_DOWN ? page : -page, shift);
        return;
    }
    case B_HOME: {
        const size_t line = LineOf(tab_->caret);
        MoveCaret(tab_->doc.line_start(line), shift, true);
        return;
    }
    case B_END: {
        const size_t line = LineOf(tab_->caret);
        MoveCaret(ContentEnd(tab_->doc, line), shift, true);
        return;
    }
    case B_BACKSPACE:
        if (tab_->caret != tab_->anchor) {
            const size_t a = std::min(tab_->caret, tab_->anchor);
            const size_t b = std::max(tab_->caret, tab_->anchor);
            ReplaceRange(a, b - a, "");
        } else if (tab_->caret > 0) {
            const size_t prev = PrevByte(tab_->flat, tab_->caret);
            ReplaceRange(prev, tab_->caret - prev, "");
        }
        return;
    case B_DELETE:
        if (tab_->caret != tab_->anchor) {
            const size_t a = std::min(tab_->caret, tab_->anchor);
            const size_t b = std::max(tab_->caret, tab_->anchor);
            ReplaceRange(a, b - a, "");
        } else if (tab_->caret < tab_->doc.size()) {
            const size_t next = NextByte(tab_->flat, tab_->caret);
            ReplaceRange(tab_->caret, next - tab_->caret, "");
        }
        return;
    case B_RETURN:
        InsertText("\n");
        return;
    case B_TAB:
        InsertText("\t");
        return;
    case B_ESCAPE:
        if (tab_->caret != tab_->anchor) {
            tab_->anchor = tab_->caret;
            Invalidate();
        }
        return;
    default:
        break;
    }

    if (cmd || ctrl) {
        return;
    }
    if (ch >= 0x20 && ch != 0x7f) {
        InsertText(std::string_view(bytes, static_cast<size_t>(numBytes)));
        if (host_ != nullptr && numBytes == 1 &&
            (bytes[0] == '.' || bytes[0] == '>' || bytes[0] == ':')) {
            host_->OnComplete(2, bytes[0]);
        }
    }
}

void EditorView::MouseDown(BPoint where) {
    if (tab_ == nullptr) {
        return;
    }
    MakeFocus(true);
    int32 buttons = 0;
    int32 clicks = 1;
    if (Window() != nullptr && Window()->CurrentMessage() != nullptr) {
        Window()->CurrentMessage()->FindInt32("buttons", &buttons);
        Window()->CurrentMessage()->FindInt32("clicks", &clicks);
    }
    if (buttons != 0 && (buttons & B_PRIMARY_MOUSE_BUTTON) == 0) {
        return;
    }
    const size_t offset = OffsetAt(where);
    const bool shift = (modifiers() & B_SHIFT_KEY) != 0;
    const bool ctrl = (modifiers() & B_CONTROL_KEY) != 0;
    if (clicks < 2 && ctrl) {
        if (!shift) {
            tab_->anchor = offset;
        }
        tab_->caret = offset;
        tab_->preferred_x = CaretX();
        Invalidate();
        if (host_ != nullptr) {
            host_->OnGoToDefinition();
        }
        return;
    }
    if (clicks >= 3) {
        SelectLine(LineOf(offset), shift);
        return;
    }
    if (clicks == 2) {
        size_t a = offset;
        size_t b = offset;
        const std::string& text = tab_->flat;
        if (a < text.size() && IsWord(static_cast<unsigned char>(text[a]))) {
            while (a > 0) {
                const size_t prev = PrevByte(text, a);
                if (!IsWord(static_cast<unsigned char>(text[prev]))) {
                    break;
                }
                a = prev;
            }
            while (b < text.size() && IsWord(static_cast<unsigned char>(text[b]))) {
                b = NextByte(text, b);
            }
        } else if (b < text.size()) {
            b = NextByte(text, b);
        }
        if (!shift) {
            tab_->anchor = a;
        }
        tab_->caret = b;
        tab_->preferred_x = CaretX();
        Invalidate();
        return;
    }
    if (!shift) {
        tab_->anchor = offset;
    }
    tab_->caret = offset;
    tab_->preferred_x = CaretX();
    dragging_ = true;
    SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    Invalidate();
}

void EditorView::MouseUp(BPoint /*where*/) { dragging_ = false; }

void EditorView::MouseMoved(BPoint where, uint32 /*code*/, const BMessage* drag) {
    if (tab_ == nullptr) {
        return;
    }
    if (!dragging_) {
        if (host_ != nullptr) {
            host_->OnHoverAt(OffsetAt(where));
        }
        return;
    }
    int32 buttons = B_PRIMARY_MOUSE_BUTTON;
    if (drag == nullptr && Window() != nullptr && Window()->CurrentMessage() != nullptr) {
        Window()->CurrentMessage()->FindInt32("buttons", &buttons);
    }
    if ((buttons & B_PRIMARY_MOUSE_BUTTON) == 0) {
        dragging_ = false;
        return;
    }
    BPoint at = where;
    const BRect bounds = Bounds();
    float dx = 0.f;
    float dy = 0.f;
    if (where.y > bounds.bottom) {
        dy = line_h_;
    } else if (where.y < bounds.top) {
        dy = -line_h_;
    }
    if (where.x > bounds.right) {
        dx = cell_ * 4.f;
    } else if (where.x < bounds.left) {
        dx = -cell_ * 4.f;
    }
    if (dx != 0.f || dy != 0.f) {
        const float old_x = bounds.left;
        const float old_y = bounds.top;
        ScrollBy(dx, dy);
        at.x += Bounds().left - old_x;
        at.y += Bounds().top - old_y;
    }
    tab_->caret = OffsetAt(at);
    tab_->preferred_x = CaretX();
    RevealCaret();
    Invalidate();
}

}  // namespace haikuide
