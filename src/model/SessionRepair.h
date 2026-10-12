#pragma once

#include "Session.h"

namespace meridian
{

/** Brings a loaded session document back inside the schema's invariants.

    Session files are plain XML: a damaged disk, a crash in another program
    or a hand edit can leave values no code path would ever create (a zero
    sample rate, negative clip lengths, NaN levels, duplicate ids, routing
    loops, sends to tracks that no longer exist). Everything downstream (the
    engine, the UI, the remote tools) relies on those invariants, so load()
    runs this once and every later stage can trust the tree.

    Repairs are conservative: values are clamped into range, unusable nodes
    (a clip with no length, an unknown child type) are removed, and routes
    that would loop fall back to the master bus. Returns one plain-English
    line per repair; empty when the document was already valid. */
juce::StringArray repairSession (Session&);

/** Checks every invariant repairSession() establishes, written independently
    of it so each cross-checks the other. Returns a description of the first
    violation, or an empty string if the document is valid. Used by the self
    test after every repair, fuzzed load and remote-tool call. */
juce::String findSessionProblem (const Session&);

} // namespace meridian
