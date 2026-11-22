/* HaikuIDE document model.
 * Piece table, line index, undo, UTF-16 positions.
 * No Haiku headers: this file builds and tests on Linux.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#ifndef HAIKUIDE_DOCUMENT_HPP
#define HAIKUIDE_DOCUMENT_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace haikuide {

enum class NewlineStyle {
    Lf,
    Crlf,
    Cr
};

/* Logical text is UTF-8 with LF line endings.
 * Positions on the wire (LSP, the vscode API) are UTF-16 code units.
 * A code point above U+FFFF counts as two UTF-16 units.
 * A UTF-16 offset that lands on a trailing surrogate snaps to the
 * start byte of that code point.
 */
class Document {
public:
    static Document from_text(std::string_view text);
    static Document empty();

    bool insert(size_t byte_offset, std::string_view text);
    bool erase(size_t byte_offset, size_t byte_length);
    /* Erase and insert as one undo step. A typed-over selection uses this. */
    bool replace(size_t byte_offset, size_t byte_length, std::string_view text);

    bool undo();
    bool redo();
    bool can_undo() const;
    bool can_redo() const;

    size_t size() const;
    uint64_t version() const;
    NewlineStyle newline_style() const;

    std::string text() const;
    std::string slice(size_t byte_offset, size_t byte_length) const;

    size_t line_count() const;
    size_t line_start(size_t line) const;

    size_t byte_to_utf16(size_t byte_offset) const;
    size_t utf16_to_byte(size_t utf16_offset) const;

    Document();

private:
    struct Piece {
        bool add;
        size_t start;
        size_t length;
    };

    struct Snap {
        std::vector<Piece> pieces;
        size_t size;
    };

    size_t split_at(size_t byte_offset);
    void insert_at(size_t byte_offset, std::string_view text);
    void erase_at(size_t byte_offset, size_t byte_length);
    void rebuild_lines();
    void remember();
    void restore(const Snap& snap);

    std::string original_;
    std::string added_;
    std::vector<Piece> pieces_;
    std::vector<size_t> line_starts_;
    std::vector<Snap> undo_;
    std::vector<Snap> redo_;
    size_t size_ = 0;
    uint64_t version_ = 0;
    NewlineStyle newlines_ = NewlineStyle::Lf;
};

}  // namespace haikuide

#endif
