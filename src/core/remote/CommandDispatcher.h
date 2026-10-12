#pragma once

#include "RemoteCommand.h"

#include <string>
#include <string_view>

namespace meridian::remote
{

struct ActionResult
{
    bool ok = false;
    std::string message;

    static ActionResult success (std::string text) { return { true, std::move (text) }; }
    static ActionResult failure (std::string text) { return { false, std::move (text) }; }
};

/** What the remote-control layer is allowed to do to the application.

    The application implements this on the message thread; tests implement it
    with a fake. Keeping the surface this narrow is deliberate: every capability
    granted to a network client is listed here and nowhere else. */
class RemoteActions
{
public:
    virtual ~RemoteActions() = default;

    virtual ActionResult play() = 0;
    virtual ActionResult stop() = 0;
    virtual ActionResult record() = 0;
    virtual ActionResult toggleWindow() = 0;
    virtual ActionResult toggleTrackMute (int trackNumber) = 0;                    ///< 1-based
    virtual ActionResult setTrackVolume (int trackNumber, double decibels) = 0;   ///< 1-based
    virtual ActionResult bounce() = 0;                                           ///< starts a mixdown

    /** A JSON object describing transport and tracks (returned verbatim by STATUS). */
    virtual std::string statusJson() = 0;
};

/** Parses one text message, executes it against `actions`, and returns the
    JSON reply:

        {"ok":true,"command":"MUTE_TRACK_2","message":"Track 2 \"Bass\" muted"}
        {"ok":false,"input":"MUTE_TRACK_X","error":"MUTE_TRACK_<X> needs a track number ..."}

    Must be called on the thread that owns application state (the message thread). */
std::string handleRemoteMessage (std::string_view message, RemoteActions& actions);

} // namespace meridian::remote
