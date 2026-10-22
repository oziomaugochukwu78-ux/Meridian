#include "SessionBackups.h"

#include <cstring>

namespace meridian::backups
{
namespace
{
juce::String prefixFor (const juce::File& sessionFile)
{
    return sessionFile.getFileNameWithoutExtension() + ".bak.";
}

bool isBackupOf (const juce::File& f, const juce::File& sessionFile)
{
    return f.getFileName().startsWith (prefixFor (sessionFile)) && f.hasFileExtension (Session::fileExtension);
}

/** Sortable to the millisecond, so names order exactly like their times. */
juce::String timestamp (juce::Time t)
{
    return t.formatted ("%Y%m%d-%H%M%S") + "-" + juce::String (t.getMilliseconds()).paddedLeft ('0', 3);
}
constexpr int stampLength = 19; // "20261013-091500-123"
constexpr const char* sinceKey = "since=";

/** Orders backup names by time, then by the counter added when two land in the same
    millisecond ("...-123.meridian", then "...-123-1.meridian", "...-123-2", ... "-10").
    Plain name order gets that wrong: '.' sorts after '-', and "-10" before "-2". */
struct BackupOrder
{
    juce::String stamp;
    int counter = 0;

    BackupOrder (const juce::File& f, const juce::File& sessionFile)
    {
        const auto rest = f.getFileNameWithoutExtension().substring (prefixFor (sessionFile).length());
        stamp = rest.substring (0, stampLength);
        if (rest.length() > stampLength + 1 && rest[stampLength] == '-')
            counter = rest.substring (stampLength + 1).getIntValue();
    }

    bool isNewerThan (const BackupOrder& other) const
    {
        if (stamp != other.stamp)
            return stamp > other.stamp;
        return counter > other.counter;
    }
};
} // namespace

juce::File folderFor (const juce::File& sessionFile)
{
    return sessionFile.getParentDirectory().getChildFile (folderName);
}

juce::Array<juce::File> list (const juce::File& sessionFile)
{
    juce::Array<juce::File> result;
    for (const auto& f : folderFor (sessionFile).findChildFiles (juce::File::findFiles, false))
        if (isBackupOf (f, sessionFile))
            result.add (f);

    // Newest first. Names carry the time, so this does not depend on file-system timestamps.
    std::sort (result.begin(), result.end(), [&sessionFile] (const juce::File& a, const juce::File& b)
    {
        return BackupOrder (a, sessionFile).isNewerThan (BackupOrder (b, sessionFile));
    });
    return result;
}

juce::Result write (const Session& session, int keep)
{
    const auto sessionFile = session.getSessionFile();
    const auto folder = folderFor (sessionFile);
    if (! folder.isDirectory() && ! folder.createDirectory())
        return juce::Result::fail ("Could not create " + folder.getFullPathName() + ".");

    const auto base = prefixFor (sessionFile) + timestamp (juce::Time::getCurrentTime());
    auto target = folder.getChildFile (base + Session::fileExtension);
    for (int n = 1; target.exists(); ++n) // two backups within a millisecond: still in order
        target = folder.getChildFile (base + "-" + juce::String (n) + Session::fileExtension);

    juce::String error;
    if (! session.writeTo (target, error))
        return juce::Result::fail (error);

    prune (sessionFile, keep);
    return juce::Result::ok();
}

void prune (const juce::File& sessionFile, int keep)
{
    const auto all = list (sessionFile);
    for (int i = std::max (1, keep); i < all.size(); ++i)
        all.getReference (i).deleteFile();
}

juce::File markerFor (const juce::File& sessionFile)
{
    return sessionFile.getParentDirectory().getChildFile ("." + sessionFile.getFileNameWithoutExtension() + ".open");
}

void markOpen (const juce::File& sessionFile)
{
    // "since" is when the file on disk was last current (opened or saved), to the millisecond
    // in the same form as backup names: a newer backup holds work that was never saved.
    const auto now = juce::Time::getCurrentTime();
    markerFor (sessionFile).replaceWithText ("Meridian has had this session open since " + now.toISO8601 (true) + ".\n"
                                             "If Meridian is not running, it did not close normally.\n"
                                             + sinceKey + timestamp (now) + "\n");
}

void markClosed (const juce::File& sessionFile)
{
    markerFor (sessionFile).deleteFile();
}

bool wasLeftOpen (const juce::File& sessionFile)
{
    return markerFor (sessionFile).existsAsFile();
}

juce::File findRecovery (const juce::File& sessionFile)
{
    if (! wasLeftOpen (sessionFile) || ! sessionFile.existsAsFile())
        return {};
    const auto all = list (sessionFile);
    if (all.isEmpty())
        return {};
    const auto& newest = all.getReference (0);

    // Compare the backup's name (its time to the millisecond) with when the session was last
    // opened or saved. File-system times are not enough: some (Linux, through JUCE) are whole
    // seconds, so a backup made in the same second as the save would look no newer.
    juce::String since;
    for (const auto& line : juce::StringArray::fromLines (markerFor (sessionFile).loadFileAsString()))
        if (line.startsWith (sinceKey))
            since = line.substring ((int) std::strlen (sinceKey)).trim();
    const auto stamp = newest.getFileName().substring (prefixFor (sessionFile).length()).substring (0, since.length());
    if (since.length() == stampLength && stamp.length() == stampLength)
        return stamp >= since ? newest : juce::File();

    // A marker from before the stamp was recorded: fall back to file times.
    return newest.getLastModificationTime() > sessionFile.getLastModificationTime() ? newest : juce::File();
}

} // namespace meridian::backups
