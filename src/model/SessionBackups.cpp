#include "SessionBackups.h"

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
    std::sort (result.begin(), result.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getFileName() > b.getFileName();
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
    markerFor (sessionFile).replaceWithText ("Meridian has had this session open since "
                                             + juce::Time::getCurrentTime().toISO8601 (true) + ".\n"
                                             "If Meridian is not running, it did not close normally.\n");
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
    return newest.getLastModificationTime() > sessionFile.getLastModificationTime() ? newest : juce::File();
}

} // namespace meridian::backups
