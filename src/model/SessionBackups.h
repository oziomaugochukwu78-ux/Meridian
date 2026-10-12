#pragma once

#include "Session.h"

namespace meridian::backups
{

/** Session safety: rotating backups and crash detection.

    Backups are snapshots of the session as it is in memory, unsaved changes
    included, written atomically to
        <session folder>/Session File Backups/<name>.bak.<timestamp>.meridian
    The newest `keep` are kept; older ones are deleted.

    Crash detection: while a session is open, a marker file sits next to it
    (".<name>.open"). A clean close removes it. If it is still there when the
    session is next opened, the previous run ended without closing (a crash, a
    killed process, a power cut), and if a backup is newer than the session
    file, that backup holds work that was never saved: it is offered back.

    Message thread only. */

constexpr const char* folderName = "Session File Backups";
constexpr int defaultKeep = 5;

juce::File folderFor (const juce::File& sessionFile);

/** Every backup of this session, newest first. */
juce::Array<juce::File> list (const juce::File& sessionFile);

/** Writes a snapshot of the session now, then deletes all but the newest `keep`. */
juce::Result write (const Session&, int keep = defaultKeep);

/** Deletes all but the newest `keep` backups. */
void prune (const juce::File& sessionFile, int keep = defaultKeep);

juce::File markerFor (const juce::File& sessionFile);
void markOpen (const juce::File& sessionFile);
void markClosed (const juce::File& sessionFile);
bool wasLeftOpen (const juce::File& sessionFile);

/** The backup to offer after a crash: only when the session was left open and
    its newest backup is newer than the session file. Otherwise an invalid File. */
juce::File findRecovery (const juce::File& sessionFile);

} // namespace meridian::backups
