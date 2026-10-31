// Generation settings file (SPEC §6.5). See AiSettings.h.

#include "ai/AiSettings.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdlib>
#include <memory>

#if JUCE_LINUX || JUCE_MAC || JUCE_BSD
 #include <sys/stat.h>
#endif

namespace vibesynth {

namespace {

// Linux has no app-data folder in JUCE's sense (PropertiesFile uses
// ~/<folderName>), so name the XDG one; macOS and Windows use the
// platform folder with "Pulsar" inside.
juce::PropertiesFile::Options fileOptions()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "Pulsar";
    o.filenameSuffix = ".settings";
    o.osxLibrarySubFolder = "Application Support";
   #if JUCE_LINUX || JUCE_BSD
    o.folderName = ".config/Pulsar";
   #else
    o.folderName = "Pulsar";
   #endif
    o.storageFormat = juce::PropertiesFile::storeAsXML;
    o.millisecondsBeforeSaving = -1; // only save() writes
    o.processLock = nullptr;
    return o;
}

std::string readString (juce::PropertiesFile& f, const char* key, const std::string& fallback)
{
    const auto v = f.getValue (key, juce::String (fallback)).trim();
    return v.isEmpty() ? fallback : v.toStdString();
}

double readTemperature (juce::PropertiesFile& f, const char* key, double fallback)
{
    if (! f.containsKey (key))
        return fallback;
    const double t = f.getDoubleValue (key, fallback);
    return t >= 0.0 && t <= 2.0 ? t : fallback;
}

} // namespace

AiSettings::AiSettings()
{
    juce::PropertiesFile f (fileOptions());
    AiConfig d;
    config_.endpoint = readString (f, "endpoint", d.endpoint);
    config_.generateModel = readString (f, "generateModel", d.generateModel);
    config_.mutateModel = readString (f, "mutateModel", d.mutateModel);
    config_.retryModel = readString (f, "retryModel", d.retryModel);
    config_.generateTemperature = readTemperature (f, "generateTemperature", d.generateTemperature);
    config_.mutateTemperature = readTemperature (f, "mutateTemperature", d.mutateTemperature);
    config_.retryTemperature = readTemperature (f, "retryTemperature", d.retryTemperature);
    config_.reasoningEffort = f.containsKey ("reasoningEffort") ? f.getValue ("reasoningEffort").trim().toStdString()
                                                               : d.reasoningEffort;
    savedKey_ = f.getValue ("apiKey").trim().toStdString();
}

std::string AiSettings::apiKey() const
{
    if (keyFromEnvironment())
        return juce::String (std::getenv (kEnvKey)).trim().toStdString();
    return savedKey_;
}

bool AiSettings::keyFromEnvironment() const
{
    const char* env = std::getenv (kEnvKey);
    return env != nullptr && juce::String (env).trim().isNotEmpty();
}

bool AiSettings::save() const
{
    juce::PropertiesFile f (fileOptions());
    f.setValue ("endpoint", juce::String (config_.endpoint));
    f.setValue ("generateModel", juce::String (config_.generateModel));
    f.setValue ("mutateModel", juce::String (config_.mutateModel));
    f.setValue ("retryModel", juce::String (config_.retryModel));
    f.setValue ("generateTemperature", config_.generateTemperature);
    f.setValue ("mutateTemperature", config_.mutateTemperature);
    f.setValue ("retryTemperature", config_.retryTemperature);
    f.setValue ("reasoningEffort", juce::String (config_.reasoningEffort));
    f.setValue ("apiKey", juce::String (savedKey_));
    if (! f.save())
        return false;
   #if JUCE_LINUX || JUCE_MAC || JUCE_BSD
    // The file holds the key: owner read/write only.
    ::chmod (f.getFile().getFullPathName().toRawUTF8(), S_IRUSR | S_IWUSR);
   #endif
    return true;
}

std::string AiSettings::filePath() const
{
    return fileOptions().getDefaultFile().getFullPathName().toStdString();
}

} // namespace vibesynth

namespace vibesynth {

std::string pulsarDataFolder()
{
    return fileOptions().getDefaultFile().getParentDirectory().getFullPathName().toStdString();
}

} // namespace vibesynth
