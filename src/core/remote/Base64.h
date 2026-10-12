#pragma once

#include <cstddef>
#include <string>

namespace meridian::remote
{

/** Standard Base64 (RFC 4648, with padding). */
std::string base64Encode (const void* data, size_t numBytes);

} // namespace meridian::remote
