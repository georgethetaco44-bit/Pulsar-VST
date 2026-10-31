// M9: user patch folder and generation history (see PatchLibrary.h).

#include "patch/PatchLibrary.h"

#include <algorithm>

#include "ai/AiSettings.h"

namespace vibesynth {

namespace {

juce::File dataFolder()
{
    return juce::File (juce::String (pulsarDataFolder()));
}

// History files sort by name = by time: "20261030-181500-Name.pulsar".
juce::Array<juce::File> historyFilesNewestFirst (const juce::File& folder)
{
    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.pulsar");
    std::sort (files.begin(), files.end(),
               [] (const juce::File& a, const juce::File& b) { return a.getFileName() > b.getFileName(); });
    return files;
}

juce::Time timeFromHistoryName (const juce::String& fileName)
{
    // yyyymmdd-hhmmss
    if (fileName.length() < 15 || fileName[8] != '-')
        return {};
    auto num = [&fileName] (int from, int len) { return fileName.substring (from, from + len).getIntValue(); };
    return juce::Time (num (0, 4), num (4, 2) - 1, num (6, 2), num (9, 2), num (11, 2), num (13, 2), 0, true);
}

juce::String legalName (const juce::String& name)
{
    auto n = juce::File::createLegalFileName (name.trim()).trim();
    return n.isEmpty() ? juce::String ("Untitled") : n.substring (0, 64);
}

} // namespace

juce::File PatchLibrary::userFolder()
{
    return dataFolder().getChildFile ("Patches");
}

juce::File PatchLibrary::historyFolder()
{
    return dataFolder().getChildFile ("Generated");
}

juce::Array<juce::File> PatchLibrary::listPatches (const juce::File& folder)
{
    auto files = folder.findChildFiles (juce::File::findFiles, false, "*.pulsar");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b)
               { return a.getFileNameWithoutExtension().compareIgnoreCase (b.getFileNameWithoutExtension()) < 0; });
    return files;
}

std::vector<PatchLibrary::HistoryEntry> PatchLibrary::listHistory (const juce::File& folder, int limit)
{
    std::vector<HistoryEntry> entries;
    for (const auto& file : historyFilesNewestFirst (folder))
    {
        if ((int) entries.size() >= limit)
            break;
        if (file.getSize() > (juce::int64) Patch::kMaxFileBytes)
            continue;
        HistoryEntry e;
        e.file = file;
        e.time = timeFromHistoryName (file.getFileName());
        const auto parsed = parsePatch (file.loadFileAsString().toStdString());
        if (parsed.ok())
        {
            e.name = juce::String (parsed.patch.name);
            e.prompt = juce::String (parsed.patch.prompt);
        }
        if (e.name.isEmpty())
            e.name = file.getFileNameWithoutExtension().fromFirstOccurrenceOf ("-", false, false)
                                                       .fromFirstOccurrenceOf ("-", false, false);
        entries.push_back (std::move (e));
    }
    return entries;
}

juce::File PatchLibrary::userPatchFile (const juce::File& folder, const juce::String& name)
{
    return folder.getChildFile (legalName (name) + ".pulsar");
}

juce::File PatchLibrary::saveUserPatch (const juce::File& folder, const juce::String& name, Patch patch,
                                        bool overwrite, juce::String& error)
{
    const auto file = userPatchFile (folder, name);
    if (file.exists() && ! overwrite)
    {
        error = "a patch named " + file.getFileNameWithoutExtension() + " already exists";
        return {};
    }
    if (! folder.createDirectory())
    {
        error = "could not create " + folder.getFullPathName();
        return {};
    }
    patch.name = name.trim().isNotEmpty() ? name.trim().toStdString() : patch.name;
    if (! file.replaceWithText (juce::String (serializePatch (patch))))
    {
        error = "could not write " + file.getFullPathName();
        return {};
    }
    return file;
}

juce::File PatchLibrary::addToHistory (const juce::File& folder, const Patch& patch, juce::Time now,
                                       juce::String& error, int limit)
{
    if (! folder.createDirectory())
    {
        error = "could not create " + folder.getFullPathName();
        return {};
    }
    const auto stem = now.formatted ("%Y%m%d-%H%M%S-") + legalName (juce::String (patch.name));
    auto file = folder.getChildFile (stem + ".pulsar");
    for (int n = 2; file.exists() && n < 100; ++n)
        file = folder.getChildFile (stem + "-" + juce::String (n) + ".pulsar");
    if (! file.replaceWithText (juce::String (serializePatch (patch))))
    {
        error = "could not write " + file.getFullPathName();
        return {};
    }
    pruneHistory (folder, limit);
    return file;
}

void PatchLibrary::pruneHistory (const juce::File& folder, int limit)
{
    const auto files = historyFilesNewestFirst (folder);
    for (int i = std::max (0, limit); i < files.size(); ++i)
        files[i].deleteFile();
}

} // namespace vibesynth
