#include "Sha1.h"

#include <cstring>

namespace meridian::remote
{
namespace
{
inline uint32_t rotl (uint32_t x, int n) noexcept { return (x << n) | (x >> (32 - n)); }

void processBlock (uint32_t h[5], const uint8_t* block) noexcept
{
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t (block[i * 4]) << 24) | (uint32_t (block[i * 4 + 1]) << 16)
             | (uint32_t (block[i * 4 + 2]) << 8) | uint32_t (block[i * 4 + 3]);
    for (int i = 16; i < 80; ++i)
        w[i] = rotl (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

    for (int i = 0; i < 80; ++i)
    {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }

        const uint32_t temp = rotl (a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl (b, 30);
        b = a;
        a = temp;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
} // namespace

std::array<uint8_t, 20> sha1 (const void* data, size_t numBytes) noexcept
{
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };

    const auto* bytes = static_cast<const uint8_t*> (data);
    size_t offset = 0;
    for (; offset + 64 <= numBytes; offset += 64)
        processBlock (h, bytes + offset);

    // Final block(s): remaining bytes, 0x80, zero padding, 64-bit big-endian bit length.
    uint8_t tail[128] = {};
    const size_t remaining = numBytes - offset;
    if (remaining > 0)
        std::memcpy (tail, bytes + offset, remaining);
    tail[remaining] = 0x80;

    const size_t tailLength = remaining + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t bitLength = uint64_t (numBytes) * 8u;
    for (int i = 0; i < 8; ++i)
        tail[tailLength - 1 - size_t (i)] = uint8_t (bitLength >> (8 * i));

    processBlock (h, tail);
    if (tailLength == 128)
        processBlock (h, tail + 64);

    std::array<uint8_t, 20> digest {};
    for (int i = 0; i < 5; ++i)
    {
        digest[size_t (i * 4)]     = uint8_t (h[i] >> 24);
        digest[size_t (i * 4 + 1)] = uint8_t (h[i] >> 16);
        digest[size_t (i * 4 + 2)] = uint8_t (h[i] >> 8);
        digest[size_t (i * 4 + 3)] = uint8_t (h[i]);
    }
    return digest;
}

} // namespace meridian::remote
