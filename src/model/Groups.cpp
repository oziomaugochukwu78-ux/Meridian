#include "Groups.h"

namespace meridian::groups
{
namespace
{
juce::String joinIds (const juce::Array<int>& trackIds)
{
    juce::StringArray parts;
    for (const int id : trackIds)
        parts.add (juce::String (id));
    return parts.joinIntoString (",");
}

/** The members as written, before any checking: tokens that are positive whole numbers. */
juce::Array<int> parseIds (const juce::String& text, bool* allValid = nullptr)
{
    juce::Array<int> result;
    bool valid = true;
    for (const auto& token : juce::StringArray::fromTokens (text, ",", {}))
    {
        const auto trimmed = token.trim();
        if (trimmed.isNotEmpty() && trimmed.containsOnly ("0123456789") && trimmed.length() < 10 && trimmed.getIntValue() > 0)
            result.add (trimmed.getIntValue());
        else
            valid = false;
    }
    if (allValid != nullptr)
        *allValid = valid;
    return result;
}
} // namespace

juce::ValueTree groupsNode (const juce::ValueTree& sessionState)
{
    return sessionState.getChildWithName (ids::GROUPS);
}

juce::ValueTree create (Session& session, const juce::String& name, const juce::Array<int>& trackIds, bool mix, bool edit,
                        juce::UndoManager* um)
{
    juce::Array<int> valid;
    for (const int id : trackIds)
        if (session.getTrackById (id).isValid())
            valid.addIfNotAlreadyThere (id);
    if (valid.size() < 2)
        return {};

    auto state = session.getState();
    auto list = groupsNode (state);
    if (! list.isValid())
    {
        list = juce::ValueTree (ids::GROUPS);
        state.appendChild (list, um);
    }

    juce::ValueTree g (ids::GROUP);
    g.setProperty (ids::id, session.allocateId(), nullptr);
    g.setProperty (ids::name, name.trim().isNotEmpty() ? name.trim() : "Group " + juce::String (list.getNumChildren() + 1), nullptr);
    g.setProperty (ids::active, true, nullptr);
    g.setProperty (ids::mix, mix, nullptr);
    g.setProperty (ids::edit, edit, nullptr);
    g.setProperty (ids::members, joinIds (valid), nullptr);
    list.appendChild (g, um);
    return g;
}

void remove (Session& session, const juce::ValueTree& group, juce::UndoManager* um)
{
    if (auto list = groupsNode (session.getState()); list.isValid())
        list.removeChild (group, um);
}

juce::ValueTree findByName (const Session& session, const juce::String& name)
{
    for (const auto& g : groupsNode (session.getState()))
        if (g[ids::name].toString().equalsIgnoreCase (name.trim()))
            return g;
    return {};
}

juce::Array<int> members (const juce::ValueTree& group)
{
    return parseIds (group[ids::members].toString());
}

juce::Array<int> linkedTracks (const Session& session, int trackId, bool mix)
{
    juce::Array<int> result;
    for (const auto& g : groupsNode (session.getState()))
    {
        if (! isActive (g) || ! (mix ? linksMix (g) : linksEdit (g)))
            continue;
        const auto memberIds = members (g);
        if (! memberIds.contains (trackId))
            continue;
        for (const int other : memberIds)
            if (other != trackId && session.getTrackById (other).isValid())
                result.addIfNotAlreadyThere (other);
    }
    return result;
}

juce::Array<int> expandForEdit (const Session& session, const juce::Array<int>& trackIds)
{
    auto result = trackIds;
    for (const int id : trackIds)
        for (const int other : linkedTracks (session, id, false))
            result.addIfNotAlreadyThere (other);
    return result;
}

juce::String namesFor (const Session& session, int trackId)
{
    juce::StringArray names;
    for (const auto& g : groupsNode (session.getState()))
        if (members (g).contains (trackId))
            names.add (g[ids::name].toString() + (isActive (g) ? "" : " (off)"));
    return names.joinIntoString (", ");
}

juce::StringArray repair (Session& session)
{
    juce::StringArray notes;
    auto list = groupsNode (session.getState());
    if (! list.isValid())
        return notes;

    for (int i = list.getNumChildren(); --i >= 0;)
    {
        auto g = list.getChild (i);
        const auto where = "group \"" + g[ids::name].toString() + "\"";
        if (! g.hasType (ids::GROUP))
        {
            notes.add ("groups: removed an unknown <" + g.getType().toString() + "> element");
            list.removeChild (i, nullptr);
            continue;
        }
        juce::Array<int> kept;
        for (const int id : parseIds (g[ids::members].toString()))
            if (session.getTrackById (id).isValid())
                kept.addIfNotAlreadyThere (id);
        if (kept.size() < 2)
        {
            notes.add (where + ": removed (fewer than two tracks left in it)");
            list.removeChild (i, nullptr);
            continue;
        }
        if (joinIds (kept) != g[ids::members].toString())
        {
            notes.add (where + ": members " + g[ids::members].toString() + " -> " + joinIds (kept));
            g.setProperty (ids::members, joinIds (kept), nullptr);
        }
        if (g[ids::name].toString().trim().isEmpty())
        {
            notes.add (where + ": unnamed, now \"Group " + juce::String (i + 1) + "\"");
            g.setProperty (ids::name, "Group " + juce::String (i + 1), nullptr);
        }
    }
    return notes;
}

juce::String findProblem (const Session& session)
{
    // Members that are not (or no longer) tracks are not a problem: they are
    // ignored, and an undone track deletion brings its group membership back.
    for (const auto& g : groupsNode (session.getState()))
    {
        if (! g.hasType (ids::GROUP))
            return "an unknown <" + g.getType().toString() + "> in the groups";
        bool allValid = true;
        parseIds (g[ids::members].toString(), &allValid);
        if (! allValid)
            return "group \"" + g[ids::name].toString() + "\" members " + g[ids::members].toString();
        if (g[ids::name].toString().trim().isEmpty())
            return "a group without a name";
    }
    return {};
}

juce::ValueTree findById (const Session& session, int groupId)
{
    return groupsNode (session.getState()).getChildWithProperty (ids::id, groupId);
}

} // namespace meridian::groups
