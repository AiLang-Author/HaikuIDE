/* Content-Length framing, the same wire format as LSP.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#ifndef HAIKUIDE_RPC_HPP
#define HAIKUIDE_RPC_HPP

#include <string>
#include <string_view>

namespace haikuide {

std::string EncodeFrame(std::string_view json);

/* Pulls one body out of a stream buffer. Leaves an incomplete frame in place.
 * Returns false when more bytes are required. Sets err on a malformed header. */
bool TakeFrame(std::string* buffer, std::string* body, std::string* err);

}  // namespace haikuide

#endif
