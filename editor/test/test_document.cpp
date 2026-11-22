/* Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#include "document.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int g_failed = 0;

void expect(bool cond, const char* name) {
    if (cond) {
        std::cout << "ok  " << name << "\n";
        return;
    }
    std::cout << "FAIL " << name << "\n";
    ++g_failed;
}

void test_insert_erase() {
    haikuide::Document doc = haikuide::Document::from_text("hello");
    expect(doc.insert(5, " world"), "insert at end");
    expect(doc.text() == "hello world", "text after insert");
    expect(doc.insert(5, ","), "insert in middle");
    expect(doc.text() == "hello, world", "text after middle insert");
    expect(doc.erase(5, 1), "erase comma");
    expect(doc.text() == "hello world", "text after erase");
    expect(!doc.insert(100, "x"), "insert past end fails");
    expect(!doc.erase(0, 100), "erase past end fails");
    expect(doc.text() == "hello world", "failed edits leave text");
    expect(doc.version() == 3, "version counts committed edits");
}

void test_undo_across_piece_boundary() {
    haikuide::Document doc = haikuide::Document::from_text("abcdefghij");
    expect(doc.insert(4, "XYZ"), "insert splits original piece");
    expect(doc.text() == "abcdXYZefghij", "text across three pieces");
    expect(doc.erase(3, 4), "erase spans a piece boundary");
    expect(doc.text() == "abcefghij", "text after spanning erase");
    expect(doc.undo(), "undo spanning erase");
    expect(doc.text() == "abcdXYZefghij", "text restored to split state");
    expect(doc.undo(), "undo the split insert");
    expect(doc.text() == "abcdefghij", "text restored to original");
    expect(doc.redo(), "redo insert");
    expect(doc.text() == "abcdXYZefghij", "redo brings the split back");
    expect(doc.can_undo(), "undo available after redo");
}

void test_lines_and_newlines() {
    haikuide::Document doc = haikuide::Document::from_text("ab\ncd\n");
    expect(doc.line_count() == 3, "trailing newline opens a new line");
    expect(doc.line_start(0) == 0, "line 0");
    expect(doc.line_start(1) == 3, "line 1");
    expect(doc.line_start(2) == 6, "line 2");

    haikuide::Document crlf = haikuide::Document::from_text("a\r\nb\r\n");
    expect(crlf.newline_style() == haikuide::NewlineStyle::Crlf, "crlf recorded");
    expect(crlf.text() == "a\nb\n", "crlf stored as lf");
    expect(crlf.line_count() == 3, "crlf line count");
}

void test_utf16_non_bmp() {
    /* U+1F600 GRINNING FACE is F0 9F 98 80, one code point, two UTF-16 units. */
    const std::string raw = std::string("a") + "\xF0\x9F\x98\x80" + "b";
    haikuide::Document doc = haikuide::Document::from_text(raw);
    expect(doc.size() == 6, "utf-8 size of a + emoji + b");
    expect(doc.byte_to_utf16(0) == 0, "utf16 at byte 0");
    expect(doc.byte_to_utf16(1) == 1, "utf16 at emoji start");
    expect(doc.byte_to_utf16(5) == 3, "utf16 after emoji");
    expect(doc.byte_to_utf16(6) == 4, "utf16 at end");
    expect(doc.utf16_to_byte(0) == 0, "byte of utf16 0");
    expect(doc.utf16_to_byte(1) == 1, "byte of first surrogate");
    expect(doc.utf16_to_byte(2) == 1, "trailing surrogate snaps to emoji start");
    expect(doc.utf16_to_byte(3) == 5, "byte of 'b'");
    expect(doc.utf16_to_byte(4) == 6, "byte past last unit");
}

void test_replace_is_one_undo() {
    haikuide::Document doc = haikuide::Document::from_text("hello world");
    expect(doc.replace(6, 5, "there"), "replace word");
    expect(doc.text() == "hello there", "text after replace");
    expect(doc.version() == 1, "replace is one version step");
    expect(doc.undo(), "undo replace");
    expect(doc.text() == "hello world", "undo restores both sides");
    expect(!doc.can_undo(), "replace did not leave a second undo step");
    expect(doc.redo(), "redo replace");
    expect(doc.text() == "hello there", "redo replace text");
    expect(!doc.replace(100, 1, "z"), "replace past end fails");
    expect(doc.text() == "hello there", "failed replace leaves text");
}

void test_line_index_10mb() {
    constexpr size_t kBytes = 10u * 1024u * 1024u;
    constexpr size_t kStride = 64;
    std::string raw(kBytes, 'x');
    for (size_t i = kStride - 1; i < raw.size(); i += kStride) {
        raw[i] = '\n';
    }
    const size_t newlines = kBytes / kStride;
    const auto t0 = std::chrono::steady_clock::now();
    haikuide::Document doc = haikuide::Document::from_text(raw);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    expect(doc.size() == kBytes, "10MB size");
    expect(doc.line_count() == newlines + 1, "10MB line count");
    expect(doc.line_start(1) == kStride, "second line starts after one stride");
    expect(ms < 50, "10MB line index under 50ms");
    std::cout << "    line index " << ms << " ms\n";
}

}  // namespace

int main() {
    test_insert_erase();
    test_undo_across_piece_boundary();
    test_lines_and_newlines();
    test_utf16_non_bmp();
    test_replace_is_one_undo();
    test_line_index_10mb();
    if (g_failed != 0) {
        std::cout << g_failed << " failed\n";
        return 1;
    }
    std::cout << "all passed\n";
    return 0;
}
