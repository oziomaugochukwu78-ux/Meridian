#include "CommandParser.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>

namespace meridian::remote
{
namespace
{
constexpr double maxVolumeDb = 12.0;
constexpr double silenceThresholdDb = -144.0;
constexpr size_t maxCommandLength = 128;

std::string_view trim (std::string_view s)
{
    auto isSpace = [] (char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (! s.empty() && isSpace (s.front()))
        s.remove_prefix (1);
    while (! s.empty() && isSpace (s.back()))
        s.remove_suffix (1);
    return s;
}

std::string toUpperAscii (std::string_view s)
{
    std::string out (s);
    for (auto& c : out)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char> (c - 'a' + 'A');
    return out;
}

bool startsWith (std::string_view s, std::string_view prefix)
{
    return s.substr (0, prefix.size()) == prefix;
}

std::optional<int> parseTrackNumber (std::string_view s)
{
    if (s.empty() || s.size() > 4)
        return std::nullopt;

    int value = 0;
    for (const char c : s)
    {
        if (c < '0' || c > '9')
            return std::nullopt;
        value = value * 10 + (c - '0');
    }

    if (value < 1 || value > maxTrackNumber)
        return std::nullopt;
    return value;
}

/** Locale-independent decimal parser: [+-]digits[.digits], or [+-]INF. */
std::optional<double> parseDecimal (std::string_view s)
{
    bool negative = false;
    if (! s.empty() && (s.front() == '+' || s.front() == '-'))
    {
        negative = s.front() == '-';
        s.remove_prefix (1);
    }

    if (s == "INF" || s == "INFINITY")
        return negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();

    // Digits are collected into an exact integer mantissa and divided by a
    // power of ten once at the end; that single division is correctly
    // rounded, so "3.25" yields exactly 3.25 rather than 3.2500000000000004.
    uint64_t mantissa = 0;
    int digits = 0;
    int fractionDigits = 0;
    bool seenPoint = false;

    for (const char c : s)
    {
        if (c == '.' && ! seenPoint)
        {
            seenPoint = true;
        }
        else if (c >= '0' && c <= '9')
        {
            if (++digits > 15) // beyond any meaningful dB precision; keeps the mantissa exact
                return std::nullopt;
            mantissa = mantissa * 10 + static_cast<uint64_t> (c - '0');
            if (seenPoint)
                ++fractionDigits;
        }
        else
        {
            return std::nullopt;
        }
    }

    if (digits == 0)
        return std::nullopt;

    static constexpr double powersOfTen[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8,
                                              1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15 };
    const double value = static_cast<double> (mantissa) / powersOfTen[fractionDigits];
    return negative ? -value : value;
}

ParseResult fail (std::string message)
{
    return { std::nullopt, std::move (message) };
}

ParseResult succeed (Command c)
{
    return { c, {} };
}
} // namespace

std::string Command::toString() const
{
    switch (type)
    {
        case Type::play:         return "PLAY";
        case Type::stop:         return "STOP";
        case Type::record:       return "RECORD";
        case Type::toggleWindow: return "TOGGLE_WINDOW";
        case Type::status:       return "STATUS";
        case Type::help:         return "HELP";
        case Type::muteTrack:    return "MUTE_TRACK_" + std::to_string (track);
        case Type::setVolume:
        {
            std::string value = "-INF";
            if (std::isfinite (volumeDb))
            {
                char buffer[32];
                const auto r = std::to_chars (buffer, buffer + sizeof (buffer), volumeDb);
                value.assign (buffer, r.ptr);
            }
            return "SET_VOLUME_TRACK_" + std::to_string (track) + "_" + value + "dB";
        }
    }
    return {};
}

ParseResult parseCommand (std::string_view text)
{
    const auto trimmed = trim (text);
    if (trimmed.empty())
        return fail ("Empty command. Send HELP for the list of commands.");
    if (trimmed.size() > maxCommandLength)
        return fail ("Command is too long.");

    const auto upper = toUpperAscii (trimmed);

    if (upper == "PLAY")          return succeed ({ Command::Type::play });
    if (upper == "STOP")          return succeed ({ Command::Type::stop });
    if (upper == "RECORD")        return succeed ({ Command::Type::record });
    if (upper == "TOGGLE_WINDOW") return succeed ({ Command::Type::toggleWindow });
    if (upper == "STATUS")        return succeed ({ Command::Type::status });
    if (upper == "HELP")          return succeed ({ Command::Type::help });

    constexpr std::string_view mutePrefix = "MUTE_TRACK_";
    if (startsWith (upper, mutePrefix))
    {
        const auto track = parseTrackNumber (std::string_view (upper).substr (mutePrefix.size()));
        if (! track)
            return fail ("MUTE_TRACK_<X> needs a track number from 1 to " + std::to_string (maxTrackNumber) + ", e.g. MUTE_TRACK_3.");

        Command c { Command::Type::muteTrack };
        c.track = *track;
        return succeed (c);
    }

    constexpr std::string_view volumePrefix = "SET_VOLUME_TRACK_";
    if (startsWith (upper, volumePrefix))
    {
        constexpr std::string_view usage = "Use SET_VOLUME_TRACK_<X>_<Y>dB, e.g. SET_VOLUME_TRACK_2_-6.5dB.";

        const std::string_view rest = std::string_view (upper).substr (volumePrefix.size());
        const auto separator = rest.find ('_');
        if (separator == std::string_view::npos)
            return fail (std::string ("Missing volume value. ") + std::string (usage));

        const auto track = parseTrackNumber (rest.substr (0, separator));
        if (! track)
            return fail ("Invalid track number. " + std::string (usage));

        auto valueText = rest.substr (separator + 1);
        if (valueText.size() >= 2 && valueText.substr (valueText.size() - 2) == "DB")
            valueText.remove_suffix (2);

        const auto value = parseDecimal (valueText);
        if (! value)
            return fail ("Invalid volume value '" + std::string (valueText) + "'. " + std::string (usage));

        if (*value > maxVolumeDb)
            return fail ("Volume must be at most +12 dB (the top of the fader).");

        Command c { Command::Type::setVolume };
        c.track = *track;
        c.volumeDb = *value <= silenceThresholdDb ? -std::numeric_limits<double>::infinity() : *value;
        return succeed (c);
    }

    return fail ("Unknown command '" + std::string (trimmed) + "'. Send HELP for the list of commands.");
}

std::string commandReference()
{
    return "PLAY                         start playback\n"
           "STOP                         stop playback or recording\n"
           "RECORD                       record on all record-armed tracks\n"
           "TOGGLE_WINDOW                switch between the Edit and Mix windows\n"
           "MUTE_TRACK_<X>               toggle mute on track X (1 = first track)\n"
           "SET_VOLUME_TRACK_<X>_<Y>dB   ramp track X's fader to Y dB (-INF, or -144 .. +12)\n"
           "STATUS                       transport state and every track as JSON\n"
           "HELP                         this list";
}

} // namespace meridian::remote
