/* Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT. */
#include "rpc.hpp"

#include <cctype>

namespace haikuide {
namespace {

bool HeaderLength(std::string_view headers, size_t* length, std::string* err) {
    bool found = false;
    size_t i = 0;
    while (i < headers.size()) {
        size_t end = headers.find("\r\n", i);
        if (end == std::string_view::npos) {
            end = headers.size();
        }
        const std::string_view line = headers.substr(i, end - i);
        const size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string name(line.substr(0, colon));
            for (char& c : name) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (name == "content-length") {
                std::string_view raw = line.substr(colon + 1);
                while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front())) != 0) {
                    raw.remove_prefix(1);
                }
                if (raw.empty()) {
                    *err = "empty Content-Length";
                    return false;
                }
                size_t value = 0;
                for (unsigned char c : raw) {
                    if (c < '0' || c > '9') {
                        *err = "bad Content-Length";
                        return false;
                    }
                    value = value * 10 + static_cast<size_t>(c - '0');
                }
                *length = value;
                found = true;
            }
        }
        if (end == headers.size()) {
            break;
        }
        i = end + 2;
    }
    if (!found) {
        *err = "missing Content-Length";
        return false;
    }
    return true;
}

}  // namespace

std::string EncodeFrame(std::string_view json) {
    std::string out = "Content-Length: ";
    out += std::to_string(json.size());
    out += "\r\n\r\n";
    out.append(json.data(), json.size());
    return out;
}

bool TakeFrame(std::string* buffer, std::string* body, std::string* err) {
    err->clear();
    const size_t sep = buffer->find("\r\n\r\n");
    if (sep == std::string::npos) {
        return false;
    }
    size_t length = 0;
    if (!HeaderLength(std::string_view(*buffer).substr(0, sep), &length, err)) {
        return false;
    }
    if (length > 16 * 1024 * 1024) {
        *err = "frame is larger than 16MB";
        return false;
    }
    const size_t body_at = sep + 4;
    if (buffer->size() < body_at + length) {
        return false;
    }
    *body = buffer->substr(body_at, length);
    buffer->erase(0, body_at + length);
    return true;
}

}  // namespace haikuide
