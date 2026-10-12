#include <remote/WebSocketProtocol.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace meridian::remote::ws;

namespace
{
/** Builds a client-to-server frame (masked unless told otherwise). */
std::string clientFrame (uint8_t opcode, const std::string& payload, bool fin = true, bool masked = true)
{
    std::string f;
    f += static_cast<char> ((fin ? 0x80 : 0x00) | opcode);

    const uint8_t maskBit = masked ? 0x80 : 0x00;
    if (payload.size() < 126)
    {
        f += static_cast<char> (maskBit | payload.size());
    }
    else if (payload.size() <= 0xFFFF)
    {
        f += static_cast<char> (maskBit | 126);
        f += static_cast<char> (payload.size() >> 8);
        f += static_cast<char> (payload.size() & 0xFF);
    }
    else
    {
        f += static_cast<char> (maskBit | 127);
        for (int shift = 56; shift >= 0; shift -= 8)
            f += static_cast<char> ((uint64_t (payload.size()) >> shift) & 0xFF);
    }

    const uint8_t mask[4] = { 0x37, 0xFA, 0x21, 0x3D };
    if (masked)
        f.append (reinterpret_cast<const char*> (mask), 4);

    for (size_t i = 0; i < payload.size(); ++i)
        f += masked ? static_cast<char> (static_cast<uint8_t> (payload[i]) ^ mask[i & 3]) : payload[i];
    return f;
}

void feed (Reader& r, const std::string& bytes) { r.feed (bytes.data(), bytes.size()); }
} // namespace

TEST_CASE ("Reader decodes a masked text frame", "[websocket]")
{
    Reader r;
    feed (r, clientFrame (0x1, "PLAY"));
    const auto e = r.next();
    REQUIRE (e.has_value());
    REQUIRE (e->type == Event::Type::text);
    REQUIRE (e->payload == "PLAY");
    REQUIRE_FALSE (r.next().has_value());
}

TEST_CASE ("Reader copes with bytes arriving one at a time", "[websocket]")
{
    Reader r;
    const auto frames = clientFrame (0x1, "STOP") + clientFrame (0x1, std::string (300, 'x'));
    for (const char c : frames)
        r.feed (&c, 1);

    auto first = r.next();
    auto second = r.next();
    REQUIRE (first->payload == "STOP");
    REQUIRE (second->payload.size() == 300);
}

TEST_CASE ("Reader reassembles fragments around an interleaved ping", "[websocket]")
{
    Reader r;
    feed (r, clientFrame (0x1, "SET_VOLUME_", false));
    feed (r, clientFrame (0x9, "hb"));
    feed (r, clientFrame (0x0, "TRACK_1_-6dB", true));

    const auto ping = r.next();
    REQUIRE (ping->type == Event::Type::ping);
    REQUIRE (ping->payload == "hb");

    const auto text = r.next();
    REQUIRE (text->type == Event::Type::text);
    REQUIRE (text->payload == "SET_VOLUME_TRACK_1_-6dB");
}

TEST_CASE ("Reader reports a close with its status code", "[websocket]")
{
    Reader r;
    feed (r, clientFrame (0x8, std::string ("\x03\xE8" "bye", 5)));
    const auto e = r.next();
    REQUIRE (e->type == Event::Type::close);
    REQUIRE (e->code == 1000);
    REQUIRE (e->payload == "bye");
}

TEST_CASE ("Reader enforces client-side protocol rules", "[websocket][security]")
{
    SECTION ("unmasked frames")
    {
        Reader r;
        feed (r, clientFrame (0x1, "PLAY", true, false));
        const auto e = r.next();
        REQUIRE (e->type == Event::Type::error);
        REQUIRE (e->code == closeCode::protocolError);
        REQUIRE (r.hasFailed());
    }
    SECTION ("binary messages")
    {
        Reader r;
        feed (r, clientFrame (0x2, "\x01\x02"));
        REQUIRE (r.next()->code == closeCode::unsupportedData);
    }
    SECTION ("oversized messages, rejected from the header alone")
    {
        Reader r (1024);
        std::string header = clientFrame (0x1, std::string (2000, 'a')).substr (0, 8);
        feed (r, header);
        REQUIRE (r.next()->code == closeCode::messageTooBig);
    }
    SECTION ("invalid UTF-8")
    {
        Reader r;
        feed (r, clientFrame (0x1, "\xC3\x28"));
        REQUIRE (r.next()->code == closeCode::invalidPayload);
    }
    SECTION ("fragmented control frames")
    {
        Reader r;
        feed (r, clientFrame (0x9, "x", false));
        REQUIRE (r.next()->code == closeCode::protocolError);
    }
    SECTION ("continuation without a start")
    {
        Reader r;
        feed (r, clientFrame (0x0, "x"));
        REQUIRE (r.next()->code == closeCode::protocolError);
    }
}

TEST_CASE ("Server frames use the shortest length encoding", "[websocket]")
{
    REQUIRE (encodeFrame (Opcode::text, "hi") == std::string ("\x81\x02hi", 4));
    const auto medium = encodeFrame (Opcode::text, std::string (200, 'a'));
    REQUIRE (static_cast<uint8_t> (medium[1]) == 126);
    REQUIRE (medium.size() == 4 + 200);
    const auto large = encodeFrame (Opcode::text, std::string (70000, 'a'));
    REQUIRE (static_cast<uint8_t> (large[1]) == 127);
    REQUIRE (large.size() == 10 + 70000);
}

TEST_CASE ("UTF-8 validation", "[websocket]")
{
    REQUIRE (isValidUtf8 ("plain ascii"));
    REQUIRE (isValidUtf8 ("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xB5"));
    REQUIRE_FALSE (isValidUtf8 ("\xC0\xAF"));          // overlong '/'
    REQUIRE_FALSE (isValidUtf8 ("\xED\xA0\x80"));      // UTF-16 surrogate
    REQUIRE_FALSE (isValidUtf8 ("\xF4\x90\x80\x80"));  // > U+10FFFF
    REQUIRE_FALSE (isValidUtf8 ("\xE2\x82"));          // truncated
}
