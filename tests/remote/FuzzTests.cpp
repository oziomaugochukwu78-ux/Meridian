// Robustness tests for everything that parses bytes from the network.
//
// The remote port accepts input from any local process, so its parsers must
// survive arbitrary bytes: no crash, no hang, no unbounded allocation, and
// every output still well-formed. These tests feed several hundred thousand
// random and mutated inputs through each parser with a fixed seed, so a
// failure is always reproducible.

#include <remote/CommandDispatcher.h>
#include <remote/CommandParser.h>
#include <remote/JsonLimits.h>
#include <remote/WebSocketProtocol.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

using namespace meridian::remote;

namespace
{
/** xorshift64*: deterministic on every platform. */
class Rng
{
public:
    explicit Rng (uint64_t seed) : state (seed != 0 ? seed : 1) {}

    uint64_t next()
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }

    size_t below (size_t n) { return n == 0 ? 0 : static_cast<size_t> (next() % n); }
    uint8_t byte() { return static_cast<uint8_t> (next() >> 56); }

private:
    uint64_t state;
};

std::string randomBytes (Rng& rng, size_t maxLength)
{
    std::string s (rng.below (maxLength + 1), '\0');
    for (auto& c : s)
        c = static_cast<char> (rng.byte());
    return s;
}

/** Random edits of a valid input: flipped bits, replaced, inserted and deleted bytes. */
std::string mutate (Rng& rng, std::string s)
{
    const auto edits = 1 + rng.below (4);
    for (size_t e = 0; e < edits; ++e)
    {
        switch (rng.below (4))
        {
            case 0: if (! s.empty()) s[rng.below (s.size())] ^= static_cast<char> (1u << rng.below (8)); break;
            case 1: if (! s.empty()) s[rng.below (s.size())] = static_cast<char> (rng.byte()); break;
            case 2: s.insert (s.begin() + static_cast<std::ptrdiff_t> (rng.below (s.size() + 1)), static_cast<char> (rng.byte())); break;
            default: if (! s.empty()) s.erase (rng.below (s.size()), 1 + rng.below (3)); break;
        }
    }
    return s;
}

std::string clientFrame (uint8_t firstByte, const std::string& payload)
{
    std::string f;
    f += static_cast<char> (firstByte);
    if (payload.size() < 126)
    {
        f += static_cast<char> (0x80 | payload.size());
    }
    else
    {
        f += static_cast<char> (0x80 | 126);
        f += static_cast<char> (payload.size() >> 8);
        f += static_cast<char> (payload.size() & 0xFF);
    }
    const uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    f.append (reinterpret_cast<const char*> (mask), 4);
    for (size_t i = 0; i < payload.size(); ++i)
        f += static_cast<char> (static_cast<uint8_t> (payload[i]) ^ mask[i & 3]);
    return f;
}

/** Feeds `bytes` in random-sized chunks and checks the Reader's output contract. */
std::vector<ws::Event> drive (Rng& rng, const std::string& bytes, size_t maxMessageBytes)
{
    ws::Reader reader (maxMessageBytes);
    std::vector<ws::Event> events;
    size_t position = 0;
    while (position < bytes.size())
    {
        const auto chunk = std::min (bytes.size() - position, 1 + rng.below (40));
        reader.feed (bytes.data() + position, chunk);
        position += chunk;
        while (auto e = reader.next())
            events.push_back (std::move (*e));
    }
    return events;
}

void checkEvents (const std::vector<ws::Event>& events, size_t maxMessageBytes)
{
    for (size_t i = 0; i < events.size(); ++i)
    {
        const auto& e = events[i];
        if (e.type == ws::Event::Type::text)
        {
            REQUIRE (e.payload.size() <= maxMessageBytes);
            REQUIRE (ws::isValidUtf8 (e.payload));
        }
        if (e.type == ws::Event::Type::ping || e.type == ws::Event::Type::pong)
            REQUIRE (e.payload.size() <= 125);
        if (e.type == ws::Event::Type::error)
            REQUIRE (i + 1 == events.size()); // the reader is dead after an error
    }
}

/** Structural JSON check for the dispatcher's replies: balanced braces outside strings. */
bool looksLikeJsonObject (const std::string& s)
{
    if (s.empty() || s.front() != '{' || s.back() != '}')
        return false;
    int depth = 0;
    bool inString = false, escaped = false;
    for (const char c : s)
    {
        if (inString)
        {
            if (escaped)        escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"')  inString = false;
            else if (static_cast<unsigned char> (c) < 0x20) return false; // raw control character in a string
            continue;
        }
        if (c == '"') inString = true;
        else if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') { if (--depth < 0) return false; }
    }
    return depth == 0 && ! inString;
}

class NullActions final : public RemoteActions
{
public:
    ActionResult play() override                         { return ActionResult::success ("playing"); }
    ActionResult stop() override                         { return ActionResult::success ("stopped"); }
    ActionResult record() override                       { return ActionResult::failure ("no armed tracks"); }
    ActionResult toggleWindow() override                 { return ActionResult::success ("Mix window"); }
    ActionResult toggleTrackMute (int) override          { return ActionResult::success ("muted"); }
    ActionResult setTrackVolume (int, double) override   { return ActionResult::success ("ramping"); }
    ActionResult bounce() override                       { return ActionResult::success ("bouncing"); }
    std::string statusJson() override                    { return "{\"transport\":\"stopped\"}"; }
};
} // namespace

//==============================================================================
TEST_CASE ("JSON nesting limit", "[json][fuzz]")
{
    CHECK_FALSE (exceedsJsonNesting (R"({"tool":"set_track","arguments":{"track":1,"volume_db":-6}})"));
    CHECK_FALSE (exceedsJsonNesting (std::string (64, '[') + std::string (64, ']')));
    CHECK (exceedsJsonNesting (std::string (65, '[')));
    CHECK (exceedsJsonNesting (std::string (64 * 1024, '[')));
    CHECK (exceedsJsonNesting (std::string (40, '{') + std::string (40, '[')));

    // Brackets inside strings, including after escaped quotes, do not count.
    CHECK_FALSE (exceedsJsonNesting ("{\"a\":\"" + std::string (500, '[') + "\"}"));
    CHECK_FALSE (exceedsJsonNesting ("{\"a\":\"\\\"" + std::string (500, '{') + "\"}"));

    // Closing brackets cannot drive the depth negative and "bank" levels.
    CHECK (exceedsJsonNesting (std::string (100, ']') + std::string (65, '[')));
}

TEST_CASE ("Strict JSON validator follows RFC 8259", "[json]")
{
    for (const char* valid : { "{}", "[]", "0", "-0", "1.5e-3", "-12.25E+2", "true", "false", "null", "\"\"",
                               " {\"a\": [1, 2, {\"b\": null}], \"c\": \"\\u00e9\\n\\\"\"} ",
                               "\"\xc3\xa9\xe2\x82\xac\"",
                               R"({"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"set_track","arguments":{"track":1}}})" })
        CHECK (isValidJson (valid));

    for (const char* invalid : { "", " ", "{", "}", "[1,]", "{\"a\":1,}", "{a:1}", "{'a':1}", "-", "-x", "01", "1.", ".5",
                                 "1e", "+1", "NaN", "Infinity", "tru", "nul", "[1 2]", "{\"a\" 1}", "\"abc", "\"\\x\"",
                                 "\"\\u12\"", "\"a\nb\"", "1 2", "{}{}", "[\"\xff\"]", "\"\xc3\"" })
        CHECK_FALSE (isValidJson (invalid));

    CHECK (isValidJson (std::string (64, '[') + std::string (64, ']')));
    CHECK_FALSE (isValidJson (std::string (65, '[') + std::string (65, ']')));
    CHECK_FALSE (isValidJson (std::string (64 * 1024, '['))); // bounded recursion: fails fast, no overflow
}

TEST_CASE ("Strict JSON validator survives random and mutated input", "[json][fuzz]")
{
    Rng rng (0x4a534f4eull);
    const std::string valid = R"({"tool":"set_send","arguments":{"track":"Vocal","slot":"A","level_db":-6.5,"pre_fader":true,"x":[1,2.5e3,null,"\u00e9"]}})";
    REQUIRE (isValidJson (valid));
    int accepted = 0;
    for (int i = 0; i < 100000; ++i)
    {
        const auto input = (i % 4 == 0) ? randomBytes (rng, 200) : mutate (rng, valid);
        if (isValidJson (input))
        {
            ++accepted;
            REQUIRE (ws::isValidUtf8 (input)); // anything accepted is safe to hand to a UTF-8 parser
            REQUIRE_FALSE (exceedsJsonNesting (input));
        }
    }
    CHECK (accepted > 100); // some mutations (e.g. a changed digit) stay valid: the check is not vacuous
}

TEST_CASE ("WebSocket Reader survives random bytes", "[websocket][fuzz]")
{
    Rng rng (0x5745424dull);
    for (int i = 0; i < 20000; ++i)
    {
        const auto bytes = randomBytes (rng, 300);
        checkEvents (drive (rng, bytes, 1024), 1024);
    }
}

TEST_CASE ("WebSocket Reader survives mutated valid traffic", "[websocket][fuzz]")
{
    Rng rng (0x4d555441ull);
    const std::string valid = clientFrame (0x81, "PLAY")
                            + clientFrame (0x89, "ping!")
                            + clientFrame (0x01, "SET_VOLUME_") + clientFrame (0x80, "TRACK_2_-6dB")
                            + clientFrame (0x81, std::string (300, 'x'))
                            + clientFrame (0x81, "{\"tool\":\"get_status\"}")
                            + clientFrame (0x88, std::string ("\x03\xe8", 2) + "bye");

    // Unmutated, chunking must not matter: every split yields the same events.
    for (int i = 0; i < 200; ++i)
    {
        const auto events = drive (rng, valid, 1024);
        REQUIRE (events.size() == 6);
        CHECK (events[0].payload == "PLAY");
        CHECK (events[2].payload == "SET_VOLUME_TRACK_2_-6dB");
        CHECK (events[5].type == ws::Event::Type::close);
    }

    for (int i = 0; i < 50000; ++i)
        checkEvents (drive (rng, mutate (rng, valid), 1024), 1024);
}

TEST_CASE ("WebSocket Reader refuses huge declared lengths without allocating them", "[websocket][fuzz]")
{
    // 64-bit length of 2^62 bytes: must fail as "too big", not try to buffer it.
    std::string frame = "\x81\xff";
    for (int shift = 56; shift >= 0; shift -= 8)
        frame += static_cast<char> ((uint64_t (1) << 62 >> shift) & 0xFF);
    frame += std::string ("\x01\x02\x03\x04", 4);

    ws::Reader reader (64 * 1024);
    reader.feed (frame.data(), frame.size());
    const auto e = reader.next();
    REQUIRE (e.has_value());
    CHECK (e->type == ws::Event::Type::error);
    CHECK (e->code == ws::closeCode::messageTooBig);
}

TEST_CASE ("HTTP request parser and handshake survive random and mutated input", "[websocket][fuzz]")
{
    Rng rng (0x48545450ull);
    const std::string valid = "GET /?token=abc HTTP/1.1\r\nHost: 127.0.0.1:47800\r\nUpgrade: websocket\r\n"
                              "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                              "Sec-WebSocket-Version: 13\r\nOrigin: http://localhost\r\n\r\n";
    ws::HandshakePolicy policy;
    policy.allowedOrigins = { "http://localhost" };
    policy.requiredToken = "abc";

    REQUIRE (ws::parseHttpRequest (valid).has_value());
    REQUIRE (ws::evaluateHandshake (*ws::parseHttpRequest (valid), policy).accepted);

    for (int i = 0; i < 30000; ++i)
    {
        const auto input = (i % 3 == 0) ? randomBytes (rng, 400) : mutate (rng, valid);
        if (const auto request = ws::parseHttpRequest (input))
        {
            const auto result = ws::evaluateHandshake (*request, policy);
            CHECK_FALSE (result.response.empty());
            const auto access = ws::checkAccess (*request, policy);
            // A request that loses its token or gains a foreign Origin is never accepted.
            if (request->target.find ("token=abc") == std::string::npos && request->header ("authorization").empty())
                CHECK_FALSE (access.accepted);
        }
    }
}

TEST_CASE ("Command parser survives random and mutated input", "[commands][fuzz]")
{
    Rng rng (0x434d4453ull);
    const char* const valid[] = { "PLAY", "STOP", "RECORD", "TOGGLE_WINDOW", "MUTE_TRACK_12",
                                  "SET_VOLUME_TRACK_3_-6.5dB", "SET_VOLUME_TRACK_1_-INF", "SET_VOLUME_TRACK_2_+12dB",
                                  "BOUNCE", "STATUS", "HELP" };

    for (int i = 0; i < 100000; ++i)
    {
        const auto input = (i % 4 == 0) ? randomBytes (rng, 160) : mutate (rng, valid[rng.below (std::size (valid))]);
        const auto result = parseCommand (input);
        if (! result.command.has_value())
        {
            CHECK_FALSE (result.error.empty());
            continue;
        }

        const auto& c = *result.command;
        if (c.type == Command::Type::muteTrack || c.type == Command::Type::setVolume)
            REQUIRE ((c.track >= 1 && c.track <= maxTrackNumber));
        if (c.type == Command::Type::setVolume)
            REQUIRE ((c.volumeDb <= 12.0 && ! std::isnan (c.volumeDb)));

        // Canonical spelling round-trips.
        const auto again = parseCommand (c.toString());
        REQUIRE (again.command.has_value());
        CHECK (again.command->type == c.type);
        CHECK (again.command->track == c.track);
    }
}

TEST_CASE ("Dispatcher replies are always well-formed JSON objects", "[commands][fuzz]")
{
    Rng rng (0x44495350ull);
    NullActions actions;
    const char* const seeds[] = { "PLAY", "MUTE_TRACK_2", "SET_VOLUME_TRACK_2_-3dB", "STATUS", "HELP",
                                  "say \"hi\"\n\t\\", "\xc3\xa9\xe2\x82\xac\xf0\x9f\x8e\xb5" };

    for (int i = 0; i < 30000; ++i)
    {
        auto input = mutate (rng, seeds[rng.below (std::size (seeds))]);
        if (! ws::isValidUtf8 (input)) // the WebSocket layer only ever delivers valid UTF-8
            continue;
        const auto reply = handleRemoteMessage (input, actions);
        REQUIRE (looksLikeJsonObject (reply));
        REQUIRE (ws::isValidUtf8 (reply));
    }
}
