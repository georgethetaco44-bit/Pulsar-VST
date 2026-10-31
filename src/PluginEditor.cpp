#include "PluginEditor.h"

#include "patch/LexiconLint.h"
#include "ui/Snapshot.h"

using namespace vibesynth::ui;

namespace
{
constexpr int kMinScalePct = 75;   // 720 x 480
constexpr int kMaxScalePct = 150;  // 1440 x 960
constexpr int kWindowChrome = 80;  // host title bar etc., for the fit-to-screen check

HeaderState::Kind kindOf (VibeSynthAudioProcessor::StatusKind k)
{
    using K = VibeSynthAudioProcessor::StatusKind;
    switch (k)
    {
        case K::Busy:    return HeaderState::Kind::Busy;
        case K::Success: return HeaderState::Kind::Success;
        case K::Error:   return HeaderState::Kind::Error;
        case K::Info:    break;
    }
    return HeaderState::Kind::Info;
}
} // namespace

VibeSynthAudioProcessorEditor::VibeSynthAudioProcessorEditor (VibeSynthAudioProcessor& p)
    : AudioProcessorEditor (p),
      processor (p),
      macros (p.apvts),
      presets (p),
      history (p),
      settings (p)
{
    setLookAndFeel (&lookAndFeel);
    setTitle ("Pulsar");

    header.onGenerate = [this] (const juce::String& prompt) { processor.generate (prompt); };
    header.onCancel = [this] { processor.cancelGeneration(); };
    header.onToggleDetail = [this] { setErrorDetailOpen (! errorOpen); };
    header.onAddKey = [this] { settings.open(); };
    errorDetail.onClose = [this] { setErrorDetailOpen (false); };

    // M11 (SPEC §9.7): Mutate, Undo, the audition strip, Change this node.
    header.onMutate = [this] { processor.startMutate(); };
    header.onUndo = [this] { processor.undo(); };
    strip.onAudition = [this] (int slot) { processor.auditionMutation (slot); };
    strip.onKeep = [this] { processor.keepMutation(); };
    strip.onDiscard = [this] { processor.discardMutation(); };
    strip.onStop = [this] { processor.cancelMutate(); };
    changePopup.onSubmit = [this] (const juce::String& id, const juce::String& text)
    {
        return processor.requestNodeChange (id, text);
    };
    nodes.addMouseListener (this, false);

    static const char* names[numPages] = { "Presets", "Code", "History" };
    static const char* tips[numPages] = { "Built-in and saved presets", "The active patch's code (read-only)",
                                          "Every successful generation, newest first" };
    for (int i = 0; i < numPages; ++i)
    {
        auto& t = tabs[(size_t) i];
        t.setButtonText (names[i]);
        t.setTooltip (tips[i]);
        setButtonStyle (t, ButtonStyle::Tab);
        t.onClick = [this, i] { showPage (i); };
        content.addAndMakeVisible (t);
    }

    gear.setTooltip ("Settings: API key, endpoint and models");
    gear.onClick = [this] { settings.open(); };
    settings.onClose = [this] { gear.grabKeyboardFocus(); };

    content.addAndMakeVisible (header);
    content.addAndMakeVisible (macros);
    content.addAndMakeVisible (nodes);
    content.addChildComponent (presets);
    content.addChildComponent (code);
    content.addChildComponent (history);
    content.addChildComponent (presets.toolbar());
    content.addChildComponent (code.toolbar());
    content.addChildComponent (history.toolbar());
    content.addAndMakeVisible (meter);
    content.addAndMakeVisible (gear);
    content.addChildComponent (errorDetail);
    content.addChildComponent (strip);        // M11
    content.addChildComponent (changePopup);  // M11
    content.addChildComponent (settings);
    addAndMakeVisible (content);

    layoutContent();
    setFocusOrder();
    showPage (0);
    processor.addChangeListener (this);
    processor.wantNodes();
    refresh();

    // Fixed aspect, 0.75x to 1.5x, starting at the last size used but never
    // larger than the screen (the Deck is 1280 x 800).
    setResizable (true, true);
    setResizeLimits (Theme::baseWidth * kMinScalePct / 100, Theme::baseHeight * kMinScalePct / 100,
                     Theme::baseWidth * kMaxScalePct / 100, Theme::baseHeight * kMaxScalePct / 100);
    if (auto* c = getConstrainer())
        c->setFixedAspectRatio ((double) Theme::baseWidth / (double) Theme::baseHeight);
    float scale = juce::jlimit (0.75f, 1.5f, processor.getEditorScale());
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const auto area = display->userBounds;
        const float fit = juce::jmin ((area.getHeight() - (float) kWindowChrome) / (float) Theme::baseHeight,
                                      area.getWidth() / (float) Theme::baseWidth);
        scale = juce::jlimit (0.75f, 1.5f, juce::jmin (scale, fit));
    }
    setSize (juce::roundToInt ((float) Theme::baseWidth * scale), juce::roundToInt ((float) Theme::baseHeight * scale));

    startTimerHz (30);
    snapshot = SnapshotDriver::createIfRequested (*this);
}

VibeSynthAudioProcessorEditor::~VibeSynthAudioProcessorEditor()
{
    snapshot.reset();
    stopTimer();
    nodes.removeMouseListener (this);
    processor.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

//==============================================================================
void VibeSynthAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);
}

void VibeSynthAudioProcessorEditor::resized()
{
    const float scale = (float) getWidth() / (float) Theme::baseWidth;
    content.setBounds (0, 0, Theme::baseWidth, Theme::baseHeight);
    content.setTransform (juce::AffineTransform::scale (scale));
    if (snapshot == nullptr)
        processor.setEditorScale (scale);
}

void VibeSynthAudioProcessorEditor::layoutContent()
{
    // 960 x 640, 16 px margins and gutters, everything on the 8 px grid.
    constexpr int g = Theme::grid;
    header.setBounds (g * 2, g * 2, g * 116, g * 12);
    macros.setBounds (g * 2, g * 16, g * 47, g * 36);
    nodes.setBounds (g * 51, g * 16, g * 67, g * 36);

    tabRow = { g * 2, g * 54, g * 116, g * 4 };
    auto row = tabRow;
    for (auto& t : tabs)
        t.setBounds (row.removeFromLeft (g * 12));
    gear.setBounds (row.removeFromRight (g * 4));
    row.removeFromRight (g * 2);
    meter.setBounds (row.removeFromRight (g * 14).reduced (0, g / 2));
    row.removeFromRight (g * 2);
    row.removeFromLeft (g * 3);
    toolbarSlot = row;
    for (auto* bar : { &presets.toolbar(), &code.toolbar(), &history.toolbar() })
        bar->setBounds (toolbarSlot);

    pageArea = { g * 2, g * 59, g * 116, g * 19 };
    presets.setBounds (pageArea);
    code.setBounds (pageArea);
    history.setBounds (pageArea);
    strip.setBounds (pageArea);  // M11: over the pages while variations are open

    settings.setBounds (0, 0, Theme::baseWidth, Theme::baseHeight);
    placeErrorDetail();
}

void VibeSynthAudioProcessorEditor::placeErrorDetail()
{
    const auto statusInContent = header.status.getBounds() + header.getPosition();
    const int w = statusInContent.getWidth();
    errorDetail.setBounds (statusInContent.getX(), statusInContent.getBottom() + Theme::grid / 2, w,
                           errorDetail.heightForWidth (w));
}

void VibeSynthAudioProcessorEditor::setFocusOrder()
{
    int order = 1;
    auto add = [&order] (juce::Component* c) { c->setExplicitFocusOrder (order++); };
    add (&header.prompt);
    add (&header.actionButton);
    add (&header.addKeyButton);
    for (auto* c : macros.focusOrder())
        add (c);
    for (auto& t : tabs)
        add (&t);
    for (auto* c : presets.focusOrder())
        add (c);
    for (auto* c : code.focusOrder())
        add (c);
    for (auto* c : history.focusOrder())
        add (c);
    add (&gear);
    for (auto& t : tabs)
        t.setWantsKeyboardFocus (true);
}

void VibeSynthAudioProcessorEditor::showPage (int index)
{
    page = juce::jlimit (0, numPages - 1, index);
    juce::Component* pages[numPages] = { &presets, &code, &history };
    juce::Component* bars[numPages] = { &presets.toolbar(), &code.toolbar(), &history.toolbar() };
    for (int i = 0; i < numPages; ++i)
    {
        pages[i]->setVisible (i == page);
        bars[i]->setVisible (i == page);
        tabs[(size_t) i].setToggleState (i == page, juce::dontSendNotification);
    }
}

void VibeSynthAudioProcessorEditor::setErrorDetailOpen (bool open)
{
    const auto state = headerState();
    errorOpen = open && state.kind == HeaderState::Kind::Error && state.detail.isNotEmpty();
    header.status.expanded = errorOpen;
    header.status.repaint();
    if (errorOpen)
    {
        errorDetail.setText (state.detail);
        placeErrorDetail();
        errorDetail.toFront (false);
        settings.toFront (false);
    }
    errorDetail.setVisible (errorOpen);
}

bool VibeSynthAudioProcessorEditor::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (settings.isVisible())
            settings.close();
        else if (changePopup.isVisible())  // M11
            changePopup.close();
        else if (errorOpen)
            setErrorDetailOpen (false);
        else if (processor.isGenerating())
            processor.cancelGeneration();
        else
            return false;
        return true;
    }
    return false;
}

//==============================================================================
void VibeSynthAudioProcessorEditor::setHeaderOverride (std::optional<HeaderState> state)
{
    headerOverride = std::move (state);
    refresh();
}

void VibeSynthAudioProcessorEditor::setKeyOverride (std::optional<bool> hasKey)
{
    keyOverride = hasKey;
    refresh();
}

void VibeSynthAudioProcessorEditor::showSettingsForSnapshot (const juce::String& fakeKey,
                                                              const VibeSynthAudioProcessor::KeyTest& result)
{
    settings.showForSnapshot (fakeKey, result);
}

HeaderState VibeSynthAudioProcessorEditor::headerState() const
{
    if (headerOverride.has_value())
        return *headerOverride;
    HeaderState s;
    const auto& st = processor.getStatus();
    s.generating = processor.isGenerating();
    s.canGenerate = const_cast<VibeSynthAudioProcessor&> (processor).canGenerate();
    if (keyOverride.has_value())
        s.canGenerate = *keyOverride;
    s.kind = kindOf (st.kind);
    s.text = st.text;
    s.detail = st.detail;
    // M11
    s.canMutate = const_cast<VibeSynthAudioProcessor&> (processor).canMutate() && s.canGenerate;
    s.mutating = processor.getMutateView().running;
    s.canUndo = processor.canUndo();
    if (s.generating)
    {
        s.kind = HeaderState::Kind::Busy;
        s.stage = processor.getGenerationStage();
        s.seconds = (int) processor.getGenerationSeconds();
    }
    else if (s.kind == HeaderState::Kind::Busy)
    {
        // The generation ended; its result is a moment away.
        s.kind = HeaderState::Kind::Info;
    }
    return s;
}

void VibeSynthAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refresh();
}

void VibeSynthAudioProcessorEditor::refresh()
{
    const auto state = headerState();
    if (state != shownHeader)
    {
        shownHeader = state;
        header.setState (state);
        if (errorOpen)
            setErrorDetailOpen (state.kind == HeaderState::Kind::Error);
    }

    if (processor.getPatchVersion() != shownPatchVersion)
    {
        shownPatchVersion = processor.getPatchVersion();
        const auto& patch = processor.getUiPatch();
        std::array<juce::String, 8> labels;
        std::array<double, 8> defaults {};
        for (int i = 0; i < 8; ++i)
        {
            labels[(size_t) i] = i < (int) patch.macros.size() ? juce::String (patch.macros[(size_t) i].label)
                                                               : juce::String();
            defaults[(size_t) i] = patch.macroDefault (i);
        }
        macros.setPatch (juce::String (patch.name), juce::String (patch.voiceMode), labels, defaults,
                        vibesynth::macrosRead (patch));
        code.setCode (juce::String (patch.init), juce::String (patch.voice), juce::String (patch.master));
        shownNodesVersion = -1;
    }
    if (processor.getNodesVersion() != shownNodesVersion)
    {
        shownNodesVersion = processor.getNodesVersion();
        nodes.setGraph (processor.getActiveNodes(), processor.areNodesKnown(), processor.getUiPatch().graphJson);
    }
    if (processor.getActiveSource() != shownSource)
    {
        shownSource = processor.getActiveSource();
        presets.refresh();
    }
    history.refresh();
    settings.setKeyTest (processor.getKeyTest());

    // M11: the audition strip over the pages while a Mutate session is open.
    const auto& mv = mutateOverride.has_value() ? *mutateOverride : processor.getMutateView();
    strip.setView (mv);
    if (strip.isVisible() != mv.open)
    {
        strip.setVisible (mv.open);
        if (mv.open)
            strip.toFront (false);
        changePopup.toFront (false);
        settings.toFront (false);
    }
}

void VibeSynthAudioProcessorEditor::setMutateOverride (std::optional<vibesynth::MutateView> view)
{
    mutateOverride = std::move (view);
    refresh();
    strip.tick (juce::Time::getMillisecondCounterHiRes());
}

void VibeSynthAudioProcessorEditor::openChangePopup (const juce::String& nodeId, const juce::String& text)
{
    // Anchor on the node's pill when the view shows it (a coarse hit scan;
    // this path is for the snapshot tool and other programmatic opens).
    juce::Rectangle<int> anchor = nodes.getBounds().withSizeKeepingCentre (8, 8);
    bool found = false;
    for (int y = 0; y < nodes.getHeight() && ! found; y += 4)
        for (int x = 0; x < nodes.getWidth() && ! found; x += 4)
        {
            std::string hit;
            juce::Rectangle<float> pill;
            if (nodes.nodeAt ({ (float) x, (float) y }, hit, pill) && juce::String (hit) == nodeId)
            {
                anchor = pill.getSmallestIntegerContainer() + nodes.getPosition();
                found = true;
            }
        }
    const bool canChange = keyOverride.value_or (processor.canGenerate());
    changePopup.open (nodeId, anchor, canChange);
    changePopup.setText (text);
    settings.toFront (false);
}

void VibeSynthAudioProcessorEditor::mouseDown (const juce::MouseEvent& e)
{
    // M11: right-click a node -> "Change this node..." (M10's inspector
    // gets the same text box; both call requestNodeChange).
    if (e.eventComponent != &nodes || ! e.mods.isPopupMenu())
        return;
    std::string id;
    juce::Rectangle<float> pill;
    if (! nodes.nodeAt (e.position, id, pill))
        return;
    const juce::String nodeId (id);
    const bool canChange = processor.canGenerate();
    juce::PopupMenu menu;
    menu.addSectionHeader (nodeId);
    menu.addItem (1, juce::String::fromUTF8 ("Change this node\xe2\x80\xa6"), canChange);
    if (! canChange)
        menu.addItem (2, "Needs an API key (Settings)", false);
    const auto anchor = pill.getSmallestIntegerContainer() + nodes.getPosition();
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&nodes).withMousePosition(),
                        [safe = juce::Component::SafePointer<VibeSynthAudioProcessorEditor> (this), nodeId, anchor] (int r)
                        {
                            if (r == 1 && safe != nullptr)
                                safe->changePopup.open (nodeId, anchor, true);
                        });
}

void VibeSynthAudioProcessorEditor::tickMeters()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    nodes.tick (processor.nodeMeters(), now);
    meter.tick (processor.takeOutputPeak (0), processor.takeOutputPeak (1), now);
}

void VibeSynthAudioProcessorEditor::timerCallback()
{
    tickMeters();
    strip.tick (juce::Time::getMillisecondCounterHiRes());  // M11: waiting cards' seconds
    // The elapsed seconds while generating (a change, ~4 times a second).
    if (++ticks % 8 == 0 && (processor.isGenerating() || shownHeader.generating))
        refresh();
}
