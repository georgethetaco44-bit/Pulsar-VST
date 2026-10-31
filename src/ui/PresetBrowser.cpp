// M9 preset browser and history. See PresetBrowser.h.

#include "ui/PresetBrowser.h"

#include "PluginProcessor.h"
#include "ui/PulsarLookAndFeel.h"

namespace vibesynth::ui {

namespace {

constexpr int kRowH = 24;
const juce::String kDot = juce::String::fromUTF8 (" \xc2\xb7 ");

void paintCard (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (r, Theme::radius);
    g.setColour (Theme::border);
    g.drawRoundedRectangle (r.reduced (0.5f), Theme::radius, Theme::hairline);
}

void styleField (juce::TextEditor& e, const juce::String& placeholder)
{
    e.setFont (Theme::font (13.0f));
    e.setIndents (10, 0);
    e.setJustification (juce::Justification::centredLeft);
    e.setTextToShowWhenEmpty (placeholder, Theme::dimText);
    e.setSelectAllWhenFocused (true);
}

} // namespace

//==============================================================================
class PresetColumn final : public juce::Component, private juce::ListBoxModel
{
public:
    PresetColumn (juce::String t, juce::String empty) : title (std::move (t)), emptyText (std::move (empty))
    {
        list.setModel (this);
        list.setRowHeight (kRowH);
        list.setOutlineThickness (0);
        list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        list.setTitle (title + " presets");
        addAndMakeVisible (list);
    }

    void setItems (std::vector<PresetBrowser::Item> newItems)
    {
        items = std::move (newItems);
        filter (lastFilter);
    }

    void filter (const juce::String& text)
    {
        lastFilter = text.trim();
        shown.clear();
        for (const auto& it : items)
            if (lastFilter.isEmpty() || it.name.containsIgnoreCase (lastFilter))
                shown.push_back (it);
        list.updateContent();
        list.repaint();
        repaint();
    }

    void setActiveKey (const juce::String& key)
    {
        if (key == activeKey)
            return;
        activeKey = key;
        revealActive();
        list.repaint();
    }

    // Scrolls the active row into view (once the list has a size).
    void revealActive()
    {
        if (list.getHeight() <= 0)
            return;
        for (int i = 0; i < (int) shown.size(); ++i)
            if (shown[(size_t) i].key == activeKey)
                list.scrollToEnsureRowIsOnscreen (i);
    }

    void paint (juce::Graphics& g) override
    {
        using namespace Theme;
        paintCard (g, getLocalBounds().toFloat());
        auto header = getLocalBounds().reduced (grid + grid / 2, 0).removeFromTop (grid * 4);
        g.setColour (dimText);
        g.setFont (font (captionSize, Weight::SemiBold).withExtraKerningFactor (0.12f));
        g.drawText (title.toUpperCase(), header, juce::Justification::centredLeft);
        g.setFont (font (captionSize));
        g.drawText (lastFilter.isEmpty() ? juce::String ((int) items.size())
                                         : juce::String ((int) shown.size()) + " of " + juce::String ((int) items.size()),
                    header, juce::Justification::centredRight);
        if (shown.empty())
        {
            g.setFont (font (smallSize));
            g.drawFittedText (items.empty() ? emptyText : "No match", list.getBounds().reduced (grid, 0),
                              juce::Justification::centredTop, 3);
        }
    }

    void resized() override
    {
        list.setBounds (getLocalBounds().reduced (grid / 2 + 2).withTrimmedTop (grid * 4 - grid / 2));
        revealActive();
    }

    std::function<void (const PresetBrowser::Item&)> onLoad;
    juce::ListBox list;

private:
    static constexpr int grid = Theme::grid;

    int getNumRows() override { return (int) shown.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        using namespace Theme;
        if (row < 0 || row >= (int) shown.size())
            return;
        const auto& it = shown[(size_t) row];
        const bool active = it.key == activeKey;
        auto r = juce::Rectangle<int> (0, 0, width, height).reduced (2, 1).toFloat();
        if (active)
        {
            g.setColour (accent.withAlpha (0.10f));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (accent);
            g.fillRoundedRectangle (r.withWidth (2.0f).reduced (0.0f, 5.0f), 1.0f);
        }
        else if (selected && list.hasKeyboardFocus (true))
        {
            g.setColour (control);
            g.fillRoundedRectangle (r, 4.0f);
        }
        g.setColour (active ? accent : text.withAlpha (0.88f));
        g.setFont (font (13.0f, active ? Weight::Medium : Weight::Regular));
        g.drawFittedText (it.name, r.toNearestInt().reduced (grid + 2, 0), juce::Justification::centredLeft, 1, 1.0f);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override { returnKeyPressed (row); }

    void returnKeyPressed (int row) override
    {
        if (row >= 0 && row < (int) shown.size() && onLoad)
            onLoad (shown[(size_t) row]);
    }

    juce::String getTooltipForRow (int row) override
    {
        if (row < 0 || row >= (int) shown.size())
            return {};
        const auto& it = shown[(size_t) row];
        return it.builtin >= 0 ? "Load " + it.name + " (built-in)"
                               : "Load " + it.name + " (" + it.file.getFullPathName() + ")";
    }

    juce::String title, emptyText, lastFilter, activeKey;
    std::vector<PresetBrowser::Item> items, shown;
};

//==============================================================================
PresetBrowser::PresetBrowser (VibeSynthAudioProcessor& p) : processor (p)
{
    showcase = std::make_unique<PresetColumn> ("Showcase", "");
    basic = std::make_unique<PresetColumn> ("Basic", "");
    user = std::make_unique<PresetColumn> ("User", "No saved patches yet. Save keeps the current patch here.");

    std::vector<Item> first, rest;
    for (int i = 0; i < processor.getNumPrograms(); ++i)
    {
        Item it;
        it.name = processor.getProgramName (i);
        it.key = "builtin:" + juce::String (i);
        it.builtin = i;
        (i < vibesynth::numShowcasePresets() ? first : rest).push_back (it);
    }
    showcase->setItems (std::move (first));
    basic->setItems (std::move (rest));

    for (auto* c : { showcase.get(), basic.get(), user.get() })
    {
        c->onLoad = [this] (const Item& it) { load (it); };
        addAndMakeVisible (*c);
    }

    styleField (bar.search, "Search presets");
    bar.search.setTooltip ("Filter all three lists by name");
    bar.search.onTextChange = [this] { applyFilter(); };
    bar.search.onEscapeKey = [this] { bar.search.setText ({}, true); };
    styleField (bar.name, "Patch name");
    bar.name.onReturnKey = [this] { saveClicked (false); };
    bar.name.onEscapeKey = [this] { setMode (Mode::Browse); };

    for (auto* b : { &bar.open, &bar.save, &bar.del, &bar.cancel })
        setButtonStyle (*b, ButtonStyle::Ghost);
    bar.open.setTooltip ("Load a .pulsar file from anywhere (runs the full offline test)");
    bar.save.setTooltip ("Save the current patch, with the current macro positions as its defaults, to your patches");
    bar.del.setTooltip ("Delete the selected user patch (asks first)");
    bar.cancel.setTooltip ("Cancel");
    bar.open.onClick = [this] { openClicked(); };
    bar.save.onClick = [this] { setMode (Mode::Naming); };
    bar.del.onClick = [this] { setMode (Mode::ConfirmDelete); };
    bar.cancel.onClick = [this] { setMode (Mode::Browse); };
    bar.confirm.onClick = [this]
    {
        if (bar.mode == Mode::Naming)
            saveClicked (false);
        else if (bar.mode == Mode::ConfirmReplace)
            saveClicked (true);
        else if (bar.mode == Mode::ConfirmDelete)
            deleteConfirmed();
    };
    for (auto* c : std::initializer_list<juce::Component*> { &bar.search, &bar.name, &bar.open, &bar.save, &bar.del,
                                                             &bar.confirm, &bar.cancel })
        bar.addChildComponent (c);
    setMode (Mode::Browse);
    refresh();
}

PresetBrowser::~PresetBrowser() = default;

juce::Array<juce::Component*> PresetBrowser::focusOrder()
{
    return { &bar.search, &bar.open, &bar.save, &bar.del, &bar.name, &bar.confirm, &bar.cancel,
             &showcase->list, &basic->list, &user->list };
}

void PresetBrowser::visibilityChanged()
{
    if (isVisible() && ! userScanned)
        rescanUser();
}

void PresetBrowser::rescanUser()
{
    // Names only (no file is read): cheap enough for the message thread.
    userScanned = true;
    std::vector<Item> items;
    for (const auto& f : PatchLibrary::listPatches (PatchLibrary::userFolder()))
    {
        Item it;
        it.name = f.getFileNameWithoutExtension();
        it.key = f.getFullPathName();
        it.file = f;
        items.push_back (it);
    }
    user->setItems (std::move (items));
    refresh();
}

void PresetBrowser::refresh()
{
    const auto key = processor.getActiveSource();
    for (auto* c : { showcase.get(), basic.get(), user.get() })
        c->setActiveKey (key);
    bar.del.setEnabled (activeIsUserPatch());
}

bool PresetBrowser::activeIsUserPatch() const
{
    const juce::File f (processor.getActiveSource());
    return juce::File::isAbsolutePath (processor.getActiveSource()) && f.existsAsFile()
        && f.isAChildOf (PatchLibrary::userFolder());
}

void PresetBrowser::load (const Item& item)
{
    if (item.builtin >= 0)
        processor.loadBuiltin (item.builtin);
    else
        processor.loadPatchFile (item.file);
}

void PresetBrowser::applyFilter()
{
    const auto text = bar.search.getText();
    for (auto* c : { showcase.get(), basic.get(), user.get() })
        c->filter (text);
}

void PresetBrowser::setMode (Mode m)
{
    bar.mode = m;
    const bool browse = m == Mode::Browse;
    bar.search.setVisible (browse);
    bar.open.setVisible (browse);
    bar.save.setVisible (browse);
    bar.del.setVisible (browse);
    bar.name.setVisible (m == Mode::Naming);
    bar.confirm.setVisible (! browse);
    bar.cancel.setVisible (! browse);
    setButtonStyle (bar.confirm, m == Mode::ConfirmDelete ? ButtonStyle::Danger : ButtonStyle::Ghost);

    const auto active = juce::String (processor.getUiPatch().name);
    switch (m)
    {
        case Mode::Naming:
            bar.question = "Save as";
            bar.confirm.setButtonText ("Save");
            bar.confirm.setTooltip ("Save to your patches (Enter)");
            bar.name.setText (active, false);
            bar.name.grabKeyboardFocus();
            break;
        case Mode::ConfirmReplace:
            bar.question = "Replace your patch '" + pendingName + "'?";
            bar.confirm.setButtonText ("Replace");
            bar.confirm.setTooltip ("Overwrite the saved patch");
            break;
        case Mode::ConfirmDelete:
            bar.question = "Delete '" + juce::File (processor.getActiveSource()).getFileNameWithoutExtension() + "'?";
            bar.confirm.setButtonText ("Delete");
            bar.confirm.setTooltip ("Move the patch file to the trash");
            break;
        case Mode::Browse:
            bar.question.clear();
            break;
    }
    refresh();
    bar.resized();
    bar.repaint();
}

void PresetBrowser::saveClicked (bool overwrite)
{
    const auto name = overwrite ? pendingName : bar.name.getText().trim();
    if (name.isEmpty())
        return;
    // The current macro positions become the saved defaults.
    auto patch = processor.getUiPatch();
    for (int i = 0; i < 8; ++i)
        if (auto* param = processor.apvts.getParameter ("macro" + juce::String (i + 1)))
        {
            const double v = vibesynth::quantisePatchNumber (param->getValue());
            if (i >= (int) patch.macros.size() && std::abs (v - 0.5) > 1.0e-6)
                patch.macros.resize ((size_t) i + 1);
            if (i < (int) patch.macros.size())
                patch.macros[(size_t) i].defaultValue = v;
        }
    juce::String error;
    const auto file = PatchLibrary::saveUserPatch (PatchLibrary::userFolder(), name, patch, overwrite, error);
    if (file == juce::File() && ! overwrite && PatchLibrary::userPatchFile (PatchLibrary::userFolder(), name).exists())
    {
        pendingName = name;
        setMode (Mode::ConfirmReplace);
        return;
    }
    if (file == juce::File())
    {
        processor.setStatus ("Could not save: " + error, VibeSynthAudioProcessor::StatusKind::Error, error);
        setMode (Mode::Browse);
        return;
    }
    patch.name = name.toStdString();
    processor.adoptSavedPatch (file, patch);
    setMode (Mode::Browse);
    rescanUser();
}

void PresetBrowser::deleteConfirmed()
{
    const juce::File f (processor.getActiveSource());
    if (activeIsUserPatch())
    {
        const bool ok = f.moveToTrash() || f.deleteFile();
        processor.setStatus (ok ? "Deleted " + f.getFileNameWithoutExtension()
                                : "Could not delete " + f.getFullPathName(),
                             ok ? VibeSynthAudioProcessor::StatusKind::Info : VibeSynthAudioProcessor::StatusKind::Error);
    }
    setMode (Mode::Browse);
    rescanUser();
}

void PresetBrowser::openClicked()
{
    auto folder = PatchLibrary::userFolder();
    if (! folder.isDirectory())
        folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    chooser = std::make_unique<juce::FileChooser> ("Open a Pulsar patch", folder, "*.pulsar");
    juce::Component::SafePointer<PresetBrowser> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (safe != nullptr && file.existsAsFile())
                                  safe->processor.loadPatchFile (file);
                          });
}

void PresetBrowser::resized()
{
    using namespace Theme;
    auto r = getLocalBounds();
    const int w = (r.getWidth() - grid * 4) / 3;
    showcase->setBounds (r.removeFromLeft (w));
    r.removeFromLeft (grid * 2);
    basic->setBounds (r.removeFromLeft (w));
    r.removeFromLeft (grid * 2);
    user->setBounds (r);
}

void PresetBrowser::Bar::resized()
{
    using namespace Theme;
    auto r = getLocalBounds();
    const int bw = grid * 9;
    if (mode == Mode::Browse)
    {
        del.setBounds (r.removeFromRight (bw));
        r.removeFromRight (grid);
        save.setBounds (r.removeFromRight (bw));
        r.removeFromRight (grid);
        open.setBounds (r.removeFromRight (bw));
        r.removeFromRight (grid * 2);
        search.setBounds (r.removeFromLeft (juce::jmin (r.getWidth(), grid * 30)));
        return;
    }
    cancel.setBounds (r.removeFromRight (bw));
    r.removeFromRight (grid);
    confirm.setBounds (r.removeFromRight (bw));
    r.removeFromRight (grid * 2);
    if (mode == Mode::Naming)
    {
        r.removeFromLeft (grid * 8); // "Save as"
        name.setBounds (r.removeFromLeft (juce::jmin (r.getWidth(), grid * 30)));
    }
}

void PresetBrowser::Bar::paint (juce::Graphics& g)
{
    if (question.isEmpty())
        return;
    g.setColour (mode == Mode::ConfirmDelete ? Theme::warning : Theme::text);
    g.setFont (Theme::font (13.0f, Theme::Weight::Medium));
    auto r = getLocalBounds();
    if (mode == Mode::Naming)
        r = r.withWidth (Theme::grid * 8);
    else
        r = r.withRight (confirm.getX() - Theme::grid * 2);
    g.drawFittedText (question, r, juce::Justification::centredLeft, 1, 1.0f);
}

//==============================================================================
HistoryPage::HistoryPage (VibeSynthAudioProcessor& p) : processor (p)
{
    list.setModel (this);
    list.setRowHeight (Theme::grid * 5);
    list.setOutlineThickness (0);
    list.setTitle ("Generation history");
    addAndMakeVisible (list);
    setButtonStyle (bar.reveal, ButtonStyle::Ghost);
    bar.reveal.setTooltip ("Show the history folder in the file manager");
    bar.reveal.onClick = []
    {
        const auto folder = PatchLibrary::historyFolder();
        if (folder.isDirectory())
            folder.revealToUser();
    };
    bar.addAndMakeVisible (bar.reveal);
}

void HistoryPage::visibilityChanged()
{
    refresh();
}

void HistoryPage::refresh()
{
    if (! isVisible())
        return;
    const bool sourceChanged = shownSource != processor.getActiveSource();
    shownSource = processor.getActiveSource();
    if (scannedVersion != processor.getHistoryVersion())
    {
        // At most 50 small files; only while this page is shown.
        scannedVersion = processor.getHistoryVersion();
        entries = PatchLibrary::listHistory (PatchLibrary::historyFolder());
        list.updateContent();
        const int n = (int) entries.size();
        bar.summary = n == 0 ? juce::String ("No generations yet")
                             : juce::String (n) + (n == 1 ? " generation" : " generations") + kDot
                                   + "newest first" + kDot + "last " + juce::String (PatchLibrary::kHistoryLimit) + " kept";
        bar.repaint();
        repaint();
    }
    else if (! sourceChanged)
    {
        return;
    }
    list.repaint();
}

void HistoryPage::paint (juce::Graphics& g)
{
    paintCard (g, getLocalBounds().toFloat());
    if (entries.empty())
    {
        g.setColour (Theme::dimText);
        g.setFont (Theme::font (13.0f));
        g.drawFittedText ("Every successful generation is saved here, newest first. Click one to load it again.",
                          getLocalBounds().reduced (Theme::grid * 4), juce::Justification::centred, 2);
    }
}

void HistoryPage::resized()
{
    list.setBounds (getLocalBounds().reduced (Theme::grid / 2 + 2));
}

void HistoryPage::paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected)
{
    using namespace Theme;
    if (row < 0 || row >= (int) entries.size())
        return;
    const auto& e = entries[(size_t) row];
    const bool active = e.file.getFullPathName() == processor.getActiveSource();
    auto r = juce::Rectangle<int> (0, 0, width, height).reduced (2, 1).toFloat();
    if (active)
    {
        g.setColour (accent.withAlpha (0.10f));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (accent);
        g.fillRoundedRectangle (r.withWidth (2.0f).reduced (0.0f, 8.0f), 1.0f);
    }
    else if (selected && list.hasKeyboardFocus (true))
    {
        g.setColour (control);
        g.fillRoundedRectangle (r, 4.0f);
    }
    auto inner = r.toNearestInt().reduced (grid + 2, 3);
    const auto when = e.time.toMilliseconds() > 0 ? e.time.formatted ("%b %d, %H:%M") : juce::String();
    auto meta = inner.removeFromBottom (14);
    g.setColour (active ? accent : text);
    g.setFont (font (13.0f, Weight::Medium));
    g.drawFittedText (e.prompt.isNotEmpty() ? e.prompt.replaceCharacters ("\r\n", "  ") : e.name, inner,
                      juce::Justification::centredLeft, 1, 1.0f);
    g.setColour (dimText);
    g.setFont (font (captionSize));
    g.drawText (e.name + (when.isNotEmpty() ? kDot + when : juce::String()), meta, juce::Justification::centredLeft);
}

void HistoryPage::listBoxItemClicked (int row, const juce::MouseEvent&)
{
    returnKeyPressed (row);
}

void HistoryPage::returnKeyPressed (int row)
{
    if (row >= 0 && row < (int) entries.size())
        processor.loadPatchFile (entries[(size_t) row].file);
}

juce::String HistoryPage::getTooltipForRow (int row)
{
    if (row < 0 || row >= (int) entries.size())
        return {};
    return entries[(size_t) row].prompt + "\n\nLoad " + entries[(size_t) row].name + " again";
}

void HistoryPage::Bar::resized()
{
    reveal.setBounds (getLocalBounds().removeFromRight (Theme::grid * 13));
}

void HistoryPage::Bar::paint (juce::Graphics& g)
{
    g.setColour (Theme::dimText);
    g.setFont (Theme::font (Theme::smallSize));
    g.drawFittedText (summary, getLocalBounds().withTrimmedRight (Theme::grid * 14), juce::Justification::centredLeft, 1);
}

} // namespace vibesynth::ui
