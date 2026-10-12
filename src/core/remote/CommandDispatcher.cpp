#include "CommandDispatcher.h"

#include "CommandParser.h"
#include "JsonWriter.h"

namespace meridian::remote
{
namespace
{
/** Prefix of at most maxBytes that does not split a UTF-8 sequence. */
std::string_view utf8Prefix (std::string_view text, size_t maxBytes)
{
    if (text.size() <= maxBytes)
        return text;
    size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char> (text[cut]) & 0xC0) == 0x80)
        --cut;
    return text.substr (0, cut);
}

std::string errorReply (std::string_view input, std::string_view error)
{
    // Echo at most a short prefix of the input, so a reply can never be used
    // to bounce large payloads back.
    return JsonObject()
        .addBool ("ok", false)
        .addString ("input", utf8Prefix (input, 128))
        .addString ("error", error)
        .toString();
}

std::string reply (const Command& command, const ActionResult& result)
{
    JsonObject json;
    json.addBool ("ok", result.ok).addString ("command", command.toString());
    if (result.ok)
        json.addString ("message", result.message);
    else
        json.addString ("error", result.message);
    return json.toString();
}
} // namespace

std::string handleRemoteMessage (std::string_view message, RemoteActions& actions)
{
    const auto parsed = parseCommand (message);
    if (! parsed.command)
        return errorReply (message, parsed.error);

    const auto& command = *parsed.command;

    switch (command.type)
    {
        case Command::Type::play:         return reply (command, actions.play());
        case Command::Type::stop:         return reply (command, actions.stop());
        case Command::Type::record:       return reply (command, actions.record());
        case Command::Type::toggleWindow: return reply (command, actions.toggleWindow());
        case Command::Type::muteTrack:    return reply (command, actions.toggleTrackMute (command.track));
        case Command::Type::setVolume:    return reply (command, actions.setTrackVolume (command.track, command.volumeDb));
        case Command::Type::bounce:       return reply (command, actions.bounce());

        case Command::Type::status:
            return JsonObject()
                .addBool ("ok", true)
                .addString ("command", "STATUS")
                .addRaw ("status", actions.statusJson())
                .toString();

        case Command::Type::help:
            return JsonObject()
                .addBool ("ok", true)
                .addString ("command", "HELP")
                .addString ("message", commandReference())
                .toString();
    }

    return errorReply (message, "Unhandled command");
}

} // namespace meridian::remote
