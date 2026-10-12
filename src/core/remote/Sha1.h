#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace meridian::remote
{

/** SHA-1 (FIPS 180-4). Used only for the WebSocket opening handshake, where
    RFC 6455 mandates it. It is not used for anything security-sensitive. */
std::array<uint8_t, 20> sha1 (const void* data, size_t numBytes) noexcept;

} // namespace meridian::remote
