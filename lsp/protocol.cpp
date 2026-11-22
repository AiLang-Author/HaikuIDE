/* Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT. */
#include "protocol.hpp"

#include <cctype>
#include <cstdint>

namespace haikuide {
namespace {

size_t Decode(std::string_view text, size_t i, uint32_t* cp) {
    if (i >= text.size()) {
        return 0;
    }
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xe0) == 0xc0 && i + 1 < text.size()) {
        *cp = (static_cast<uint32_t>(c & 0x1f) << 6) |
              static_cast<uint32_t>(static_cast<unsigned char>(text[i + 1]) & 0x3f);
        return 2;
    }
    if ((c & 0xf0) == 0xe0 && i + 2 < text.size()) {
        *cp = (static_cast<uint32_t>(c & 0x0f) << 12) |
              (static_cast<uint32_t>(static_cast<unsigned char>(text[i + 1]) & 0x3f) << 6) |
              static_cast<uint32_t>(static_cast<unsigned char>(text[i + 2]) & 0x3f);
        return 3;
    }
    if ((c & 0xf8) == 0xf0 && i + 3 < text.size()) {
        *cp = (static_cast<uint32_t>(c & 0x07) << 18) |
              (static_cast<uint32_t>(static_cast<unsigned char>(text[i + 1]) & 0x3f) << 12) |
              (static_cast<uint32_t>(static_cast<unsigned char>(text[i + 2]) & 0x3f) << 6) |
              static_cast<uint32_t>(static_cast<unsigned char>(text[i + 3]) & 0x3f);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

int Units(uint32_t cp) { return cp > 0xFFFF ? 2 : 1; }

void AppendEncoded(std::string* out, unsigned char c) {
    char hex[] = "0123456789ABCDEF";
    out->push_back('%');
    out->push_back(hex[c >> 4]);
    out->push_back(hex[c & 0x0f]);
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

}  // namespace

Pos ByteToPos(std::string_view text, size_t byte) {
    if (byte > text.size()) {
        byte = text.size();
    }
    int line = 0;
    size_t line_start = 0;
    for (size_t i = 0; i < byte; ++i) {
        if (text[i] == '\n') {
            ++line;
            line_start = i + 1;
        }
    }
    int character = 0;
    size_t i = line_start;
    while (i < byte) {
        uint32_t cp = 0;
        const size_t n = Decode(text, i, &cp);
        if (n == 0 || i + n > byte) {
            break;
        }
        character += Units(cp);
        i += n;
    }
    return Pos{line, character};
}

size_t PosToByte(std::string_view text, int line, int character) {
    if (line < 0) {
        line = 0;
    }
    if (character < 0) {
        character = 0;
    }
    size_t i = 0;
    int seen = 0;
    while (i < text.size() && seen < line) {
        if (text[i] == '\n') {
            ++seen;
        }
        ++i;
    }
    int need = character;
    while (i < text.size() && text[i] != '\n' && need > 0) {
        uint32_t cp = 0;
        const size_t n = Decode(text, i, &cp);
        if (n == 0) {
            break;
        }
        const int units = Units(cp);
        if (units > need) {
            return i;
        }
        need -= units;
        i += n;
    }
    return i;
}

std::string PathToUri(std::string path) {
    std::string uri = "file://";
    for (unsigned char c : path) {
        const bool plain = std::isalnum(c) != 0 || c == '/' || c == '-' || c == '_' || c == '.' ||
                           c == '~';
        if (plain) {
            uri.push_back(static_cast<char>(c));
        } else {
            AppendEncoded(&uri, c);
        }
    }
    return uri;
}

std::string UriToPath(std::string uri) {
    const std::string file = "file://";
    if (uri.compare(0, file.size(), file) == 0) {
        uri = uri.substr(file.size());
        const std::string localhost = "localhost";
        if (uri.compare(0, localhost.size(), localhost) == 0) {
            uri = uri.substr(localhost.size());
        }
    }
    std::string path;
    for (size_t i = 0; i < uri.size(); ++i) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            const int hi = HexValue(uri[i + 1]);
            const int lo = HexValue(uri[i + 2]);
            if (hi >= 0 && lo >= 0) {
                path.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        path.push_back(uri[i]);
    }
    return path;
}

std::string ExtensionOf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return std::string();
    }
    std::string ext = path.substr(dot);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

}  // namespace haikuide
