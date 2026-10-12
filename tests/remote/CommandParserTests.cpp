#include <remote/CommandParser.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace meridian::remote;
using Type = Command::Type;

namespace
{
Command parseOk (const char* text)
{
    const auto result = parseCommand (text);
    INFO ("input: " << text << " error: " << result.error);
    REQUIRE (result.command.has_value());
    return *result.command;
}

std::string parseError (const char* text)
{
    const auto result = parseCommand (text);
    INFO ("input: " << text);
    REQUIRE_FALSE (result.command.has_value());
    REQUIRE_FALSE (result.error.empty());
    return result.error;
}
} // namespace

TEST_CASE ("Transport and window commands", "[parser]")
{
    REQUIRE (parseOk ("PLAY").type == Type::play);
    REQUIRE (parseOk ("STOP").type == Type::stop);
    REQUIRE (parseOk ("RECORD").type == Type::record);
    REQUIRE (parseOk ("TOGGLE_WINDOW").type == Type::toggleWindow);
    REQUIRE (parseOk ("STATUS").type == Type::status);
    REQUIRE (parseOk ("HELP").type == Type::help);
}

TEST_CASE ("Commands are case-insensitive and whitespace-tolerant", "[parser]")
{
    REQUIRE (parseOk ("  play\r\n").type == Type::play);
    REQUIRE (parseOk ("Toggle_Window").type == Type::toggleWindow);
    REQUIRE (parseOk ("mute_track_2").track == 2);
}

TEST_CASE ("MUTE_TRACK_<X>", "[parser]")
{
    const auto c = parseOk ("MUTE_TRACK_12");
    REQUIRE (c.type == Type::muteTrack);
    REQUIRE (c.track == 12);

    parseError ("MUTE_TRACK_");
    parseError ("MUTE_TRACK_0");
    parseError ("MUTE_TRACK_-1");
    parseError ("MUTE_TRACK_ABC");
    parseError ("MUTE_TRACK_10000");
}

TEST_CASE ("SET_VOLUME_TRACK_<X>_<Y>dB", "[parser]")
{
    auto c = parseOk ("SET_VOLUME_TRACK_3_-6.5dB");
    REQUIRE (c.type == Type::setVolume);
    REQUIRE (c.track == 3);
    REQUIRE (c.volumeDb == -6.5);

    REQUIRE (parseOk ("SET_VOLUME_TRACK_1_0dB").volumeDb == 0.0);
    REQUIRE (parseOk ("SET_VOLUME_TRACK_1_+3.25DB").volumeDb == 3.25);
    REQUIRE (parseOk ("SET_VOLUME_TRACK_1_-12").volumeDb == -12.0);   // suffix optional
    REQUIRE (parseOk ("SET_VOLUME_TRACK_1_12dB").volumeDb == 12.0);   // top of the fader
    REQUIRE (parseOk ("set_volume_track_1_.5db").volumeDb == 0.5);

    const auto silent = parseOk ("SET_VOLUME_TRACK_1_-INFdB");
    REQUIRE (std::isinf (silent.volumeDb));
    REQUIRE (silent.volumeDb < 0);
    REQUIRE (std::isinf (parseOk ("SET_VOLUME_TRACK_1_-300dB").volumeDb)); // below the fader floor

    parseError ("SET_VOLUME_TRACK_1_12.01dB");
    parseError ("SET_VOLUME_TRACK_1_+INFdB");
    parseError ("SET_VOLUME_TRACK_1_dB");
    parseError ("SET_VOLUME_TRACK_1_-6..5dB");
    parseError ("SET_VOLUME_TRACK_1_1e3dB");
    parseError ("SET_VOLUME_TRACK_0_-6dB");
    parseError ("SET_VOLUME_TRACK_-6dB");
}

TEST_CASE ("Unknown and degenerate input", "[parser]")
{
    parseError ("");
    parseError ("   ");
    parseError ("PLAYY");
    parseError ("DELETE_EVERYTHING");
    parseError (std::string (500, 'A').c_str());
}

TEST_CASE ("Canonical spelling round-trips", "[parser]")
{
    for (const char* text : { "PLAY", "MUTE_TRACK_4", "SET_VOLUME_TRACK_2_-6.5dB", "SET_VOLUME_TRACK_9_-INFdB" })
    {
        const auto c = parseOk (text);
        REQUIRE (c.toString() == text);
        const auto again = parseOk (c.toString().c_str());
        REQUIRE (again.toString() == c.toString());
    }
}
