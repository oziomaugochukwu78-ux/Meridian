#pragma once

#include "RemoteCommand.h"

#include <optional>
#include <string>
#include <string_view>

namespace meridian::remote
{

/** Grammar (case-insensitive, surrounding whitespace ignored):

        PLAY | STOP | RECORD | TOGGLE_WINDOW | STATUS | HELP
        MUTE_TRACK_<X>
        SET_VOLUME_TRACK_<X>_<Y>[dB]

    X is a track number from 1 to maxTrackNumber.
    Y is a decimal number such as 0, -6, -6.5, +3.25 or the word -INF. It may not
    exceed +12 dB (the top of the fader); anything at or below -144 dB means
    "fader fully down" and is stored as -infinity. The "dB" suffix is optional.
*/
struct ParseResult
{
    std::optional<Command> command;
    std::string error; ///< Set when command is empty: what was wrong, phrased for the sender.
};

inline constexpr int maxTrackNumber = 9999;

ParseResult parseCommand (std::string_view text);

/** The help text returned by the HELP command. */
std::string commandReference();

} // namespace meridian::remote
