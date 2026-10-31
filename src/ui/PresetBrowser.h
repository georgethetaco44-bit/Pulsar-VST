#pragma once

// M9 preset browser and history (SPEC §9.5).
//
// PresetBrowser: three columns, Showcase and Basic (the built-ins in
// program order) and User (the "Patches" folder next to the settings),
// filtered by the search box. A click loads: built-ins compile-only, user
// files through the full offline test. Save names the patch and writes it
// to the user folder with the current macro positions as its defaults;
// Delete (user patches only) asks first, inline. The user folder is
// listed (names only) when the page is first shown and after a change.
//
// HistoryPage: every successful generation, newest first (the newest 50
// are kept), showing its prompt; a click reloads it.

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

#include "patch/PatchLibrary.h"
#include "ui/Theme.h"

class VibeSynthAudioProcessor;

namespace vibesynth::ui {

class PresetColumn;

class PresetBrowser final : public juce::Component
{
public:
    explicit PresetBrowser (VibeSynthAudioProcessor& p);
    ~PresetBrowser() override;

    // The processor changed (active source or patch).
    void refresh();
    void rescanUser();

    void resized() override;
    void visibilityChanged() override;

    juce::Component& toolbar() { return bar; }
    juce::Array<juce::Component*> focusOrder();

    struct Item
    {
        juce::String name;
        juce::String key;      // "builtin:N" or the file's full path
        int builtin = -1;
        juce::File file;
    };

private:
    enum class Mode { Browse, Naming, ConfirmReplace, ConfirmDelete };

    struct Bar final : public juce::Component
    {
        void resized() override;
        void paint (juce::Graphics&) override;
        Mode mode = Mode::Browse;
        juce::TextEditor search, name;
        juce::TextButton open { juce::String::fromUTF8 ("Open\xe2\x80\xa6") }, save { "Save" }, del { "Delete" };
        juce::TextButton confirm, cancel { "Cancel" };
        juce::String question;
    };

    void load (const Item& item);
    void applyFilter();
    void setMode (Mode m);
    void saveClicked (bool overwrite);
    void deleteConfirmed();
    void openClicked();
    bool activeIsUserPatch() const;

    VibeSynthAudioProcessor& processor;
    std::unique_ptr<PresetColumn> showcase, basic, user;
    Bar bar;
    bool userScanned = false;
    juce::String pendingName;
    std::unique_ptr<juce::FileChooser> chooser;
};

class HistoryPage final : public juce::Component, private juce::ListBoxModel
{
public:
    explicit HistoryPage (VibeSynthAudioProcessor& p);

    void refresh();  // rescans when visible and the history changed
    void resized() override;
    void paint (juce::Graphics&) override;
    void visibilityChanged() override;

    juce::Component& toolbar() { return bar; }
    juce::Array<juce::Component*> focusOrder() { return { &list, &bar.reveal }; }

private:
    struct Bar final : public juce::Component
    {
        void resized() override;
        void paint (juce::Graphics&) override;
        juce::String summary;
        juce::TextButton reveal { "Show folder" };
    };

    int getNumRows() override { return (int) entries.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int row) override;
    juce::String getTooltipForRow (int row) override;

    VibeSynthAudioProcessor& processor;
    std::vector<PatchLibrary::HistoryEntry> entries;
    int scannedVersion = -1;
    juce::String shownSource;
    juce::ListBox list;
    Bar bar;
};

} // namespace vibesynth::ui
