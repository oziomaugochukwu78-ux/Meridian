#include <remote/Base64.h>
#include <remote/Sha1.h>
#include <remote/WebSocketProtocol.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace meridian::remote;

namespace
{
std::string hex (const std::array<uint8_t, 20>& digest)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto b : digest)
    {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

std::string sha1Hex (const std::string& s) { return hex (sha1 (s.data(), s.size())); }
} // namespace

TEST_CASE ("SHA-1 matches FIPS 180 test vectors", "[sha1]")
{
    REQUIRE (sha1Hex ("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    REQUIRE (sha1Hex ("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    // 56 bytes: the padding spills into a second block.
    REQUIRE (sha1Hex ("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
             == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    // One million 'a': many full blocks.
    REQUIRE (sha1Hex (std::string (1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE ("Base64 matches RFC 4648 test vectors", "[base64]")
{
    auto enc = [] (const std::string& s) { return base64Encode (s.data(), s.size()); };
    REQUIRE (enc ("") == "");
    REQUIRE (enc ("f") == "Zg==");
    REQUIRE (enc ("fo") == "Zm8=");
    REQUIRE (enc ("foo") == "Zm9v");
    REQUIRE (enc ("foob") == "Zm9vYg==");
    REQUIRE (enc ("fooba") == "Zm9vYmE=");
    REQUIRE (enc ("foobar") == "Zm9vYmFy");
}

TEST_CASE ("WebSocket accept key matches the RFC 6455 example", "[websocket]")
{
    REQUIRE (ws::computeAcceptKey ("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}
