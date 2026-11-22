/* Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#include "document.hpp"

#include <algorithm>
#include <utility>

namespace haikuide {
namespace {

struct Decoded {
    char32_t cp;
    size_t bytes;
};

Decoded decode_utf8(const std::string& bytes, size_t at, size_t end) {
    if (at >= end) {
        return {0xFFFD, 0};
    }
    const auto lead = static_cast<unsigned char>(bytes[at]);
    size_t need = 1;
    char32_t cp = 0;
    if (lead < 0x80) {
        return {lead, 1};
    } else if ((lead & 0xE0) == 0xC0) {
        need = 2;
        cp = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        need = 3;
        cp = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        need = 4;
        cp = lead & 0x07;
    } else {
        return {0xFFFD, 1};
    }
    if (at + need > end) {
        return {0xFFFD, 1};
    }
    for (size_t i = 1; i < need; ++i) {
        const auto cont = static_cast<unsigned char>(bytes[at + i]);
        if ((cont & 0xC0) != 0x80) {
            return {0xFFFD, 1};
        }
        cp = (cp << 6) | (cont & 0x3F);
    }
    const bool overlong =
        (need == 2 && cp < 0x80) ||
        (need == 3 && cp < 0x800) ||
        (need == 4 && cp < 0x10000);
    if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        return {0xFFFD, 1};
    }
    return {cp, need};
}

size_t utf16_units(char32_t cp) {
    return cp > 0xFFFF ? 2 : 1;
}

std::string normalize_newlines(std::string_view text, NewlineStyle* style) {
    bool saw_crlf = false;
    bool saw_lf = false;
    bool saw_cr = false;
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                saw_crlf = true;
                ++i;
            } else {
                saw_cr = true;
            }
            out.push_back('\n');
        } else {
            if (c == '\n') {
                saw_lf = true;
            }
            out.push_back(c);
        }
    }
    if (saw_crlf && !saw_lf && !saw_cr) {
        *style = NewlineStyle::Crlf;
    } else if (saw_cr && !saw_lf && !saw_crlf) {
        *style = NewlineStyle::Cr;
    } else {
        *style = NewlineStyle::Lf;
    }
    return out;
}

}  // namespace

Document::Document() { rebuild_lines(); }

Document Document::empty() { return Document(); }

Document Document::from_text(std::string_view text) {
    Document doc;
    if (text.find('\r') == std::string_view::npos) {
        doc.newlines_ = NewlineStyle::Lf;
        doc.original_.assign(text.data(), text.size());
    } else {
        doc.original_ = normalize_newlines(text, &doc.newlines_);
    }
    if (!doc.original_.empty()) {
        doc.pieces_.push_back(Piece{false, 0, doc.original_.size()});
        doc.size_ = doc.original_.size();
    }
    doc.rebuild_lines();
    return doc;
}

bool Document::insert(size_t byte_offset, std::string_view text) {
    if (byte_offset > size_) {
        return false;
    }
    if (text.empty()) {
        return true;
    }
    remember();
    insert_at(byte_offset, text);
    ++version_;
    rebuild_lines();
    return true;
}

bool Document::erase(size_t byte_offset, size_t byte_length) {
    if (byte_length == 0) {
        return byte_offset <= size_;
    }
    if (byte_offset > size_ || byte_length > size_ - byte_offset) {
        return false;
    }
    remember();
    erase_at(byte_offset, byte_length);
    ++version_;
    rebuild_lines();
    return true;
}

bool Document::replace(size_t byte_offset, size_t byte_length, std::string_view text) {
    if (byte_offset > size_ || byte_length > size_ - byte_offset) {
        return false;
    }
    if (byte_length == 0) {
        return insert(byte_offset, text);
    }
    if (text.empty()) {
        return erase(byte_offset, byte_length);
    }
    remember();
    erase_at(byte_offset, byte_length);
    insert_at(byte_offset, text);
    ++version_;
    rebuild_lines();
    return true;
}

bool Document::undo() {
    if (undo_.empty()) {
        return false;
    }
    redo_.push_back(Snap{pieces_, size_});
    restore(undo_.back());
    undo_.pop_back();
    ++version_;
    rebuild_lines();
    return true;
}

bool Document::redo() {
    if (redo_.empty()) {
        return false;
    }
    undo_.push_back(Snap{pieces_, size_});
    restore(redo_.back());
    redo_.pop_back();
    ++version_;
    rebuild_lines();
    return true;
}

bool Document::can_undo() const { return !undo_.empty(); }
bool Document::can_redo() const { return !redo_.empty(); }

size_t Document::size() const { return size_; }
uint64_t Document::version() const { return version_; }
NewlineStyle Document::newline_style() const { return newlines_; }

std::string Document::text() const { return slice(0, size_); }

std::string Document::slice(size_t byte_offset, size_t byte_length) const {
    if (byte_offset > size_) {
        return {};
    }
    if (byte_length > size_ - byte_offset) {
        byte_length = size_ - byte_offset;
    }
    std::string out;
    out.reserve(byte_length);
    size_t need = byte_length;
    size_t skip = byte_offset;
    for (const Piece& piece : pieces_) {
        if (need == 0) {
            break;
        }
        if (skip >= piece.length) {
            skip -= piece.length;
            continue;
        }
        const size_t take = std::min(piece.length - skip, need);
        const std::string& buf = piece.add ? added_ : original_;
        out.append(buf, piece.start + skip, take);
        need -= take;
        skip = 0;
    }
    return out;
}

size_t Document::line_count() const { return line_starts_.size(); }

size_t Document::line_start(size_t line) const {
    if (line >= line_starts_.size()) {
        return size_;
    }
    return line_starts_[line];
}

size_t Document::byte_to_utf16(size_t byte_offset) const {
    if (byte_offset > size_) {
        byte_offset = size_;
    }
    const std::string flat = text();
    size_t units = 0;
    size_t i = 0;
    while (i < byte_offset && i < flat.size()) {
        const Decoded dec = decode_utf8(flat, i, flat.size());
        if (dec.bytes == 0 || i + dec.bytes > byte_offset) {
            break;
        }
        units += utf16_units(dec.cp);
        i += dec.bytes;
    }
    return units;
}

size_t Document::utf16_to_byte(size_t utf16_offset) const {
    const std::string flat = text();
    size_t units = 0;
    size_t i = 0;
    while (i < flat.size() && units < utf16_offset) {
        const Decoded dec = decode_utf8(flat, i, flat.size());
        if (dec.bytes == 0) {
            break;
        }
        const size_t need = utf16_units(dec.cp);
        if (units + need > utf16_offset) {
            return i;
        }
        units += need;
        i += dec.bytes;
    }
    return i;
}

void Document::insert_at(size_t byte_offset, std::string_view text) {
    const size_t add_at = added_.size();
    added_.append(text.data(), text.size());
    const size_t index = split_at(byte_offset);
    pieces_.insert(pieces_.begin() + static_cast<std::ptrdiff_t>(index),
                   Piece{true, add_at, text.size()});
    size_ += text.size();
}

void Document::erase_at(size_t byte_offset, size_t byte_length) {
    split_at(byte_offset + byte_length);
    const size_t begin = split_at(byte_offset);
    size_t acc = byte_offset;
    size_t end = begin;
    while (end < pieces_.size() && acc < byte_offset + byte_length) {
        acc += pieces_[end].length;
        ++end;
    }
    pieces_.erase(pieces_.begin() + static_cast<std::ptrdiff_t>(begin),
                  pieces_.begin() + static_cast<std::ptrdiff_t>(end));
    size_ -= byte_length;
}

size_t Document::split_at(size_t byte_offset) {
    size_t acc = 0;
    for (size_t i = 0; i < pieces_.size(); ++i) {
        Piece& piece = pieces_[i];
        if (acc == byte_offset) {
            return i;
        }
        if (acc + piece.length > byte_offset) {
            const size_t left = byte_offset - acc;
            Piece right = piece;
            right.start += left;
            right.length -= left;
            piece.length = left;
            pieces_.insert(pieces_.begin() + static_cast<std::ptrdiff_t>(i + 1), right);
            return i + 1;
        }
        acc += piece.length;
    }
    return pieces_.size();
}

void Document::rebuild_lines() {
    line_starts_.clear();
    /* One allocation for source-shaped files (lines of 32 bytes or more).
     * Shorter lines grow the vector geometrically from here. */
    line_starts_.reserve(size_ / 32 + 2);
    line_starts_.push_back(0);
    size_t off = 0;
    for (const Piece& piece : pieces_) {
        const std::string& buf = piece.add ? added_ : original_;
        for (size_t i = 0; i < piece.length; ++i) {
            if (buf[piece.start + i] == '\n') {
                line_starts_.push_back(off + i + 1);
            }
        }
        off += piece.length;
    }
}

void Document::remember() {
    undo_.push_back(Snap{pieces_, size_});
    redo_.clear();
}

void Document::restore(const Snap& snap) {
    pieces_ = snap.pieces;
    size_ = snap.size;
}

}  // namespace haikuide
