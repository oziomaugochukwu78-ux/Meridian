#pragma once

#include <string>

namespace meridian::remote
{

/** One parsed remote-control instruction. */
struct Command
{
    enum class Type
    {
        play,           ///< PLAY
        stop,           ///< STOP
        record,         ///< RECORD
        toggleWindow,   ///< TOGGLE_WINDOW         - swap Edit <-> Mix
        muteTrack,      ///< MUTE_TRACK_<X>        - toggle mute of track X
        setVolume,      ///< SET_VOLUME_TRACK_<X>_<Y>dB
        bounce,         ///< BOUNCE                - mix the session down to an audio file
        status,         ///< STATUS                - JSON snapshot of transport + tracks
        help,           ///< HELP                  - list of commands
    };

    Type type = Type::status;
    int track = 0;           ///< 1-based track number as shown in the UI (muteTrack, setVolume).
    double volumeDb = 0.0;   ///< Target fader level; -infinity means fader fully down.

    /** Canonical spelling, e.g. "SET_VOLUME_TRACK_2_-6.5dB". */
    std::string toString() const;
};

} // namespace meridian::remote
