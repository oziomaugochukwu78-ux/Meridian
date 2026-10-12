#include "Base64.h"

#include <cstdint>

namespace meridian::remote
{

std::string base64Encode (const void* data, size_t numBytes)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    const auto* bytes = static_cast<const uint8_t*> (data);
    std::string out;
    out.reserve ((numBytes + 2) / 3 * 4);

    size_t i = 0;
    for (; i + 3 <= numBytes; i += 3)
    {
        const uint32_t v = (uint32_t (bytes[i]) << 16) | (uint32_t (bytes[i + 1]) << 8) | uint32_t (bytes[i + 2]);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }

    const size_t rest = numBytes - i;
    if (rest == 1)
    {
        const uint32_t v = uint32_t (bytes[i]) << 16;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += "==";
    }
    else if (rest == 2)
    {
        const uint32_t v = (uint32_t (bytes[i]) << 16) | (uint32_t (bytes[i + 1]) << 8);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

} // namespace meridian::remote
