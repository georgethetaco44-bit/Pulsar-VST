#pragma once

// Generation settings (SPEC §6.3, §6.5): endpoint, model IDs and
// temperatures, plus the API key.
//
// The key lives only in the user's settings file (juce::PropertiesFile in
// the "Pulsar" app-data folder: ~/.config/Pulsar on Linux, %APPDATA%\Pulsar
// on Windows, ~/Library/Application Support/Pulsar on macOS) or in the
// VIBESYNTH_API_KEY environment variable, which overrides it. It is never
// compiled in, logged or written anywhere else.
//
// Compiled into the plugin and vibesynth_cli (VIBESYNTH_AI_SOURCES), like
// Patch.cpp. Message thread or the CLI; never the audio thread. Reading
// the file is the only I/O: no network here.

#include <string>

namespace vibesynth {

struct AiConfig
{
    // SPEC §6.3 defaults (configurable, never hardcoded at the call site).
    static constexpr const char* kDefaultEndpoint = "https://api.meta.ai/v1/chat/completions";
    static constexpr const char* kDefaultModel = "muse-spark-1.3";

    std::string endpoint = kDefaultEndpoint;
    std::string generateModel = kDefaultModel;
    std::string mutateModel = kDefaultModel;
    std::string retryModel = kDefaultModel;
    double generateTemperature = 0.8;
    double mutateTemperature = 0.9;
    double retryTemperature = 0.15;
    // The service is a reasoning model whose hidden reasoning counts
    // against max_tokens (M8 batch): "low" keeps a patch inside the 30 s
    // timeout. Empty = not sent.
    std::string reasoningEffort = "low";
};

class AiSettings
{
public:
    static constexpr const char* kEnvKey = "VIBESYNTH_API_KEY";

    // Reads the settings file (missing file = defaults, no key).
    AiSettings();

    const AiConfig& config() const noexcept { return config_; }
    void setConfig (const AiConfig& c) { config_ = c; }

    // The key to use: VIBESYNTH_API_KEY if set and non-empty, otherwise
    // the saved one (possibly empty).
    std::string apiKey() const;
    bool keyFromEnvironment() const;
    bool hasKey() const { return ! apiKey().empty(); }
    // The saved key only (the settings panel's field).
    const std::string& savedKey() const noexcept { return savedKey_; }
    void setSavedKey (const std::string& key) { savedKey_ = key; }

    // Writes config + saved key to the settings file (owner-only
    // permissions on POSIX). Returns false if the file can't be written.
    bool save() const;
    // Where the file lives (for messages; never prints the key).
    std::string filePath() const;

private:
    AiConfig config_;
    std::string savedKey_;
};

// M9: the "Pulsar" app-data folder the settings file lives in (user
// patches and the generation history go next to it). Not created here.
std::string pulsarDataFolder();

} // namespace vibesynth
