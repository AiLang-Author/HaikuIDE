/* LSP positions and file URIs. UTF-16 columns, UTF-8 bytes.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#ifndef HAIKUIDE_PROTOCOL_HPP
#define HAIKUIDE_PROTOCOL_HPP

#include <cstddef>
#include <string>
#include <string_view>

namespace haikuide {

struct Pos {
    int line = 0;
    int character = 0;
};

Pos ByteToPos(std::string_view text, size_t byte);
size_t PosToByte(std::string_view text, int line, int character);

std::string PathToUri(std::string path);
std::string UriToPath(std::string uri);
std::string ExtensionOf(const std::string& path);

}  // namespace haikuide

#endif
