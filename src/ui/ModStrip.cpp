#include "ui/ModStrip.h"

#include <cmath>

#include "PluginProcessor.h"
#include "ui/PulsarLookAndFeel.h"

namespace vibesynth::ui {

namespace {

constexpr float kKnobStart = juce::MathConstants<float>::pi * 1.25f;
constexpr float kKnobEnd = juce::MathConstants<float>::pi * 2.75f;

juce::String formatSecondsShort (double s)
{
    if (s < 0.1)
        return juce::String (juce::roundToInt (s * 1000.0)) + " ms";
    if (s < 1.0)
        return juce::String (juce::roundToInt (s * 1000.0)) + " ms";
    return juce::String (s, 2) + " s";
}

} // namespace

//==============================================================================
ModAwareSlider::ModAwareSlider()
{
    syncProps();
}

void ModAwareSlider::setModulation (bool active, ModSource source, double depth, double liveNorm)
{
    const bool changed = (modActive_ != active) || (modSource_ != source)
                      || (std::abs (modDepth_ - depth) > 1.0e-5)
                      || (std::abs (liveNorm_ - liveNorm) > 1.0e-4);
    modActive_ = active;
    modSource_ = source;
    modDepth_ = depth;
    liveNorm_ = liveNorm;
    if (changed)
    {
        syncProps();
        repaint();
    }
}

void ModAwareSlider::syncProps()
{
    auto& props = getProperties();
    props.set ("modActive", modActive_);
    props.set ("modSource", static_cast<int> (modSource_));
    props.set ("modDepth", modDepth_);
    props.set ("modLiveNorm", liveNorm_);
}

bool ModAwareSlider::isOnModRing (juce::Point<float> pos) const noexcept
{
    if (! modActive_)
        return false;
    const auto bounds = getLocalBounds().toFloat();
    const float d = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto c = bounds.getCentre();
    const float arcR = d * 0.5f - 3.0f;
    const float faceR = arcR - 6.5f;
    const float dist = c.getDistanceFrom (pos);
    return dist >= faceR;
}

void ModAwareSlider::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        if (onRightClick)
            onRightClick();
        return;
    }
    if (modActive_ && (e.mods.isShiftDown() || e.mods.isAltDown() || isOnModRing (e.position)))
    {
        draggingRing_ = true;
        dragStartDepth_ = modDepth_;
        return;
    }
    draggingRing_ = false;
    juce::Slider::mouseDown (e);
}

void ModAwareSlider::mouseDrag (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        return;
    if (draggingRing_)
    {
        const double delta = (double) (-e.getDistanceFromDragStartY() + e.getDistanceFromDragStartX() * 0.5) / 110.0;
        double next = juce::jlimit (-1.0, 1.0, dragStartDepth_ + delta);
        if (std::abs (next) < 0.02)
            next = next >= 0.0 ? 0.02 : -0.02;
        modDepth_ = next;
        syncProps();
        repaint();
        if (onModDepthChanged)
            onModDepthChanged (modSource_, next);
        return;
    }
    juce::Slider::mouseDrag (e);
}

void ModAwareSlider::mouseUp (const juce::MouseEvent& e)
{
    if (draggingRing_)
    {
        draggingRing_ = false;
        return;
    }
    juce::Slider::mouseUp (e);
}

void drawKnobModulationRing (juce::Graphics& g, juce::Rectangle<float> bounds,
                             float pos, float startAngle, float endAngle,
                             juce::Slider& slider)
{
    const auto& props = slider.getProperties();
    if (! static_cast<bool> (props.getWithDefault ("modActive", false)))
        return;

    const auto src = static_cast<ModSource> (static_cast<int> (props.getWithDefault ("modSource", 0)));
    const float depth = static_cast<float> (static_cast<double> (props.getWithDefault ("modDepth", 0.0)));
    const double liveNorm = static_cast<double> (props.getWithDefault ("modLiveNorm", -1.0));
    const juce::Colour col = modSourceColour (src);

    const float d = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto c = bounds.getCentre();
    const float modR = d * 0.5f - 0.8f;
    const float span = endAngle - startAngle;
    const float baseAngle = startAngle + juce::jlimit (0.0f, 1.0f, pos) * span;
    const float targetAngle = juce::jlimit (startAngle, endAngle, baseAngle + depth * span);

    // Subtle full ring track so the user sees the draggable modulation ring.
    juce::Path ringTrack;
    ringTrack.addCentredArc (c.x, c.y, modR, modR, 0.0f, startAngle, endAngle, true);
    g.setColour (col.withAlpha (0.20f));
    g.strokePath (ringTrack, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Coloured depth arc from the knob's setting to targetAngle.
    if (std::abs (targetAngle - baseAngle) > 0.005f)
    {
        juce::Path depthArc;
        depthArc.addCentredArc (c.x, c.y, modR, modR, 0.0f,
                                juce::jmin (baseAngle, targetAngle),
                                juce::jmax (baseAngle, targetAngle), true);
        g.setColour (col.withAlpha (0.92f));
        g.strokePath (depthArc, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // End-of-depth handle on the ring.
    const juce::Point<float> endPt (c.x + modR * std::sin (targetAngle),
                                    c.y - modR * std::cos (targetAngle));
    g.setColour (col);
    g.fillEllipse (endPt.x - 2.3f, endPt.y - 2.3f, 4.6f, 4.6f);

    // Live modulated position dot moving on the ring.
    if (liveNorm >= 0.0)
    {
        const float liveAngle = startAngle + static_cast<float> (juce::jlimit (0.0, 1.0, liveNorm)) * span;
        const juce::Point<float> livePt (c.x + modR * std::sin (liveAngle),
                                         c.y - modR * std::cos (liveAngle));
        g.setColour (col.withAlpha (0.35f));
        g.fillEllipse (livePt.x - 4.0f, livePt.y - 4.0f, 8.0f, 8.0f);
        g.setColour (Theme::text);
        g.fillEllipse (livePt.x - 2.2f, livePt.y - 2.2f, 4.4f, 4.4f);
    }
}

//==============================================================================
ModPopover::ModPopover (VibeSynthAudioProcessor& processor) : processor_ (processor)
{
    setButtonStyle (closeButton_, ButtonStyle::Ghost);
    closeButton_.setTooltip ("Close modulator settings");
    closeButton_.onClick = [this] { close(); };
    addAndMakeVisible (closeButton_);

    static const char* kShortShapes[] = { "Sin", "Tri", "Saw+", "Saw-", "Sqr", "S&H", "Rnd" };
    for (int i = 0; i < static_cast<int> (LfoShape::Count); ++i)
    {
        auto& b = shapeButtons_[(size_t) i];
        b.setButtonText (kShortShapes[i]);
        b.setTooltip (juce::String ("Shape: ") + lfoShapeLabel (static_cast<LfoShape> (i)));
        setButtonStyle (b, ButtonStyle::Segment);
        b.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0)
                             | (i + 1 < static_cast<int> (LfoShape::Count) ? juce::Button::ConnectedOnRight : 0));
        b.onClick = [this, i]
        {
            if (! updating_)
                applyLfoEdit ([i] (ModLfoSpec& s) { s.shape = static_cast<LfoShape> (i); });
        };
        addChildComponent (b);
    }

    setButtonStyle (syncButton_, ButtonStyle::Segment);
    syncButton_.setClickingTogglesState (true);
    syncButton_.setTooltip ("Sync LFO rate to host tempo (1/32 to 4 bars, with dotted and triplet)");
    syncButton_.onClick = [this]
    {
        if (! updating_)
            applyLfoEdit ([this] (ModLfoSpec& s) { s.sync = syncButton_.getToggleState(); });
    };
    addChildComponent (syncButton_);

    setButtonStyle (retrigButton_, ButtonStyle::Segment);
    retrigButton_.setClickingTogglesState (true);
    retrigButton_.setTooltip ("Retrigger LFO phase on every note-on (off = free-running locked to host beat)");
    retrigButton_.onClick = [this]
    {
        if (! updating_)
            applyLfoEdit ([this] (ModLfoSpec& s) { s.retrigger = retrigButton_.getToggleState(); });
    };
    addChildComponent (retrigButton_);

    rateSlider_.setSliderStyle (juce::Slider::LinearHorizontal);
    rateSlider_.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    rateSlider_.setMouseClickGrabsKeyboardFocus (false);
    rateSlider_.onValueChange = [this]
    {
        if (updating_)
            return;
        applyLfoEdit ([this] (ModLfoSpec& s)
        {
            if (s.sync)
            {
                const int idx = juce::jlimit (0, static_cast<int> (LfoSyncDiv::Count) - 1,
                                              juce::roundToInt (rateSlider_.getValue()));
                s.syncDiv = static_cast<LfoSyncDiv> (idx);
            }
            else
            {
                s.rateHz = juce::jlimit (0.05, 30.0, rateSlider_.getValue());
            }
        });
    };
    addChildComponent (rateSlider_);

    static const char* kEnvNames[] = { "Attack", "Decay", "Sustain", "Release" };
    for (int i = 0; i < 4; ++i)
    {
        auto& k = envKnobs_[(size_t) i];
        k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        k.setRotaryParameters (kKnobStart, kKnobEnd, true);
        k.setMouseDragSensitivity (160);
        k.setMouseClickGrabsKeyboardFocus (false);
        k.setTitle (kEnvNames[i]);
        if (i == 2)
        {
            k.setRange (0.0, 1.0, 0.0);
        }
        else
        {
            k.setRange (0.001, 10.0, 0.0);
            k.setSkewFactorFromMidPoint (0.25);
        }
        k.onValueChange = [this, i]
        {
            if (updating_)
                return;
            const double v = envKnobs_[(size_t) i].getValue();
            applyEnvEdit ([i, v] (ModEnvSpec& e)
            {
                if (i == 0) e.attack = v;
                else if (i == 1) e.decay = v;
                else if (i == 2) e.sustain = v;
                else e.release = v;
            });
        };
        addChildComponent (k);
    }
}

ModPopover::~ModPopover() = default;

void ModPopover::openFor (ModSource source)
{
    source_ = source;
    setVisible (true);
    toFront (false);
    refreshFromPatch();
    resized();
    repaint();
}

void ModPopover::close()
{
    if (! isVisible())
        return;
    setVisible (false);
    if (onClose)
        onClose();
}

void ModPopover::applyLfoEdit (const std::function<void (ModLfoSpec&)>& fn)
{
    const int idx = source_ == ModSource::Lfo2 ? 1 : 0;
    ModLfoSpec spec = processor_.getUiPatch().mods.lfos[(size_t) idx];
    fn (spec);
    processor_.setModLfo (idx, spec);
}

void ModPopover::applyEnvEdit (const std::function<void (ModEnvSpec&)>& fn)
{
    const int idx = source_ == ModSource::Env2 ? 1 : 0;
    ModEnvSpec spec = processor_.getUiPatch().mods.envs[(size_t) idx];
    fn (spec);
    processor_.setModEnv (idx, spec);
}

void ModPopover::refreshFromPatch()
{
    if (! isVisible())
        return;
    updating_ = true;
    const auto& mods = processor_.getUiPatch().mods;
    const bool isLfo = (source_ == ModSource::Lfo1 || source_ == ModSource::Lfo2);

    for (auto& b : shapeButtons_)
        b.setVisible (isLfo);
    syncButton_.setVisible (isLfo);
    retrigButton_.setVisible (isLfo);
    rateSlider_.setVisible (isLfo);
    for (auto& k : envKnobs_)
        k.setVisible (! isLfo);

    if (isLfo)
    {
        const int idx = source_ == ModSource::Lfo2 ? 1 : 0;
        const ModLfoSpec& lfo = mods.lfos[(size_t) idx];
        for (int i = 0; i < static_cast<int> (LfoShape::Count); ++i)
            shapeButtons_[(size_t) i].setToggleState (static_cast<int> (lfo.shape) == i, juce::dontSendNotification);
        syncButton_.setToggleState (lfo.sync, juce::dontSendNotification);
        retrigButton_.setToggleState (lfo.retrigger, juce::dontSendNotification);
        if (lfo.sync)
        {
            rateSlider_.setSkewFactor (1.0);
            rateSlider_.setRange (0.0, static_cast<double> (LfoSyncDiv::Count) - 1.0, 1.0);
            rateSlider_.setValue (static_cast<double> (lfo.syncDiv), juce::dontSendNotification);
        }
        else
        {
            rateSlider_.setRange (0.05, 30.0, 0.0);
            rateSlider_.setSkewFactorFromMidPoint (2.5);
            rateSlider_.setValue (lfo.rateHz, juce::dontSendNotification);
        }
    }
    else
    {
        const int idx = source_ == ModSource::Env2 ? 1 : 0;
        const ModEnvSpec& env = mods.envs[(size_t) idx];
        envKnobs_[0].setValue (env.attack, juce::dontSendNotification);
        envKnobs_[1].setValue (env.decay, juce::dontSendNotification);
        envKnobs_[2].setValue (env.sustain, juce::dontSendNotification);
        envKnobs_[3].setValue (env.release, juce::dontSendNotification);
    }
    updating_ = false;
    repaint();
}

void ModPopover::paint (juce::Graphics& g)
{
    using namespace Theme;
    const auto bounds = getLocalBounds().toFloat();
    const juce::Colour col = modSourceColour (source_);

    g.setColour (panel.darker (0.15f));
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (col.withAlpha (0.75f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.25f);

    g.setColour (col);
    g.setFont (font (captionSize, Weight::SemiBold).withExtraKerningFactor (0.10f));
    g.drawText (juce::String (modSourceLabel (source_)) + " SETTINGS  (DRAG TILE TO MACRO OR KNOB)",
                headerArea_, juce::Justification::centredLeft);

    const auto& mods = processor_.getUiPatch().mods;
    if (source_ == ModSource::Lfo1 || source_ == ModSource::Lfo2)
    {
        const int idx = source_ == ModSource::Lfo2 ? 1 : 0;
        const ModLfoSpec& lfo = mods.lfos[(size_t) idx];
        const juce::String rateTxt = lfo.sync ? juce::String (lfoSyncDivLabel (lfo.syncDiv))
                                              : juce::String (lfo.rateHz, lfo.rateHz < 10.0 ? 2 : 1) + " Hz";
        g.setColour (text);
        g.setFont (font (smallSize, Weight::Medium));
        g.drawText (rateTxt, rateLabelArea_, juce::Justification::centredRight);
    }
    else
    {
        const int idx = source_ == ModSource::Env2 ? 1 : 0;
        const ModEnvSpec& env = mods.envs[(size_t) idx];
        const juce::String vals[4] = {
            "A " + formatSecondsShort (env.attack),
            "D " + formatSecondsShort (env.decay),
            "S " + juce::String (juce::roundToInt (env.sustain * 100.0)) + "%",
            "R " + formatSecondsShort (env.release)
        };
        g.setColour (dimText);
        g.setFont (font (captionSize, Weight::Medium));
        for (int i = 0; i < 4; ++i)
            g.drawText (vals[i], envLabelAreas_[i], juce::Justification::centred);
    }
}

void ModPopover::resized()
{
    using namespace Theme;
    auto r = getLocalBounds().reduced (10, 6);
    auto top = r.removeFromTop (20);
    closeButton_.setBounds (top.removeFromRight (22));
    headerArea_ = top;
    r.removeFromTop (4);

    if (source_ == ModSource::Lfo1 || source_ == ModSource::Lfo2)
    {
        auto shapeRow = r.removeFromTop (22);
        const int n = static_cast<int> (LfoShape::Count);
        const int w = shapeRow.getWidth() / n;
        for (int i = 0; i < n; ++i)
            shapeButtons_[(size_t) i].setBounds (i < n - 1 ? shapeRow.removeFromLeft (w) : shapeRow);

        r.removeFromTop (6);
        auto bottomRow = r.removeFromTop (22);
        syncButton_.setBounds (bottomRow.removeFromLeft (48));
        bottomRow.removeFromLeft (6);
        retrigButton_.setBounds (bottomRow.removeFromLeft (54));
        bottomRow.removeFromLeft (8);
        rateLabelArea_ = bottomRow.removeFromRight (64);
        bottomRow.removeFromRight (4);
        rateSlider_.setBounds (bottomRow);
    }
    else
    {
        const int cellW = r.getWidth() / 4;
        for (int i = 0; i < 4; ++i)
        {
            auto cell = r.removeFromLeft (i < 3 ? cellW : r.getWidth());
            envLabelAreas_[i] = cell.removeFromBottom (14);
            envKnobs_[(size_t) i].setBounds (cell.withSizeKeepingCentre (36, 36));
        }
    }
}

//==============================================================================
ModTile::ModTile (ModSource source, VibeSynthAudioProcessor& processor)
    : source_ (source), processor_ (processor)
{
    setRepaintsOnMouseActivity (true);
    const juce::String name = modSourceLabel (source_);
    setTooltip (name + ": click to edit settings, or drag onto any macro knob or inspector knob to modulate it.");
}

void ModTile::setSelected (bool selected)
{
    if (selected_ == selected)
        return;
    selected_ = selected;
    repaint();
}

void ModTile::setPlayhead (float phase, float value, bool voiceActive)
{
    const float p = juce::jlimit (0.0f, 1.0f, phase);
    const float v = juce::jlimit (-1.0f, 1.0f, value);
    if (std::abs (p - playhead_) < 0.004f && std::abs (v - liveVal_) < 0.008f && voiceActive_ == voiceActive)
        return;
    playhead_ = p;
    liveVal_ = v;
    voiceActive_ = voiceActive;
    repaint();
}

void ModTile::refreshFromPatch (const Patch& patch)
{
    if (source_ == ModSource::Lfo1 || source_ == ModSource::Lfo2)
        lfoSpec_ = patch.mods.lfos[source_ == ModSource::Lfo2 ? 1u : 0u];
    else
        envSpec_ = patch.mods.envs[source_ == ModSource::Env2 ? 1u : 0u];

    int count = 0;
    for (const auto& a : patch.mods.assignments)
        if (a.source == source_ && a.valid())
            ++count;
    assignCount_ = count;
    repaint();
}

void ModTile::paint (juce::Graphics& g)
{
    using namespace Theme;
    const auto bounds = getLocalBounds().toFloat();
    const juce::Colour col = modSourceColour (source_);
    const bool over = isMouseOverOrDragging();

    g.setColour (selected_ ? control.brighter (0.12f) : (over ? control.brighter (0.05f) : control));
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (selected_ ? col : (assignCount_ > 0 ? col.withAlpha (0.55f) : border));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, selected_ ? 1.4f : hairline);

    auto inner = getLocalBounds().reduced (5, 3);
    auto top = inner.removeFromTop (13);

    g.setColour (col);
    g.setFont (font (10.5f, Weight::SemiBold));
    g.drawText (modSourceLabel (source_), top.removeFromLeft (38), juce::Justification::centredLeft);

    juce::String sub;
    if (source_ == ModSource::Lfo1 || source_ == ModSource::Lfo2)
        sub = lfoSpec_.sync ? juce::String (lfoSyncDivLabel (lfoSpec_.syncDiv))
                            : juce::String (lfoSpec_.rateHz, 1) + "Hz";
    else
        sub = assignCount_ > 0 ? juce::String (assignCount_) + "x" : juce::String ("ADSR");

    g.setColour (dimText);
    g.setFont (font (9.5f, Weight::Medium));
    g.drawText (sub, top, juce::Justification::centredRight);

    const auto plot = inner.reduced (1, 2).toFloat();
    if (plot.getWidth() <= 4.0f || plot.getHeight() <= 4.0f)
        return;

    juce::Path curve;
    const int steps = 40;
    auto yForIdx = [&] (float norm01) -> float
    {
        if (source_ == ModSource::Lfo1 || source_ == ModSource::Lfo2)
        {
            const double v = LfoVoiceState::evalShape (lfoSpec_.shape, norm01, 0, 0x13579BDFu);
            return plot.getCentreY() - static_cast<float> (v) * (plot.getHeight() * 0.44f);
        }
        // ADSR shape preview (0..0.25 A, 0.25..0.5 D, 0.5..0.75 S, 0.75..1.0 R)
        const float sus = static_cast<float> (juce::jlimit (0.0, 1.0, envSpec_.sustain));
        float env = 0.0f;
        if (norm01 < 0.25f)
            env = norm01 / 0.25f;
        else if (norm01 < 0.5f)
            env = 1.0f - (1.0f - sus) * ((norm01 - 0.25f) / 0.25f);
        else if (norm01 < 0.75f)
            env = sus;
        else
            env = sus * (1.0f - (norm01 - 0.75f) / 0.25f);
        return plot.getBottom() - env * (plot.getHeight() * 0.88f);
    };

    for (int i = 0; i <= steps; ++i)
    {
        const float t = static_cast<float> (i) / static_cast<float> (steps);
        const float x = plot.getX() + t * plot.getWidth();
        const float y = yForIdx (t);
        if (i == 0)
            curve.startNewSubPath (x, y);
        else
            curve.lineTo (x, y);
    }

    g.setColour (col.withAlpha (0.80f));
    g.strokePath (curve, juce::PathStrokeType (1.35f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Moving playhead on the tile.
    const float px = plot.getX() + juce::jlimit (0.0f, 1.0f, playhead_) * plot.getWidth();
    const float py = yForIdx (juce::jlimit (0.0f, 1.0f, playhead_));
    g.setColour (col.withAlpha (0.35f));
    g.drawVerticalLine (juce::roundToInt (px), plot.getY(), plot.getBottom());
    g.setColour (text);
    g.fillEllipse (px - 2.2f, py - 2.2f, 4.4f, 4.4f);
}

void ModTile::mouseDown (const juce::MouseEvent&)
{
    draggingTile_ = false;
}

void ModTile::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingTile_ && e.getDistanceFromDragStart() >= 4)
    {
        draggingTile_ = true;
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
            container->startDragging ("mod:" + juce::String (static_cast<int> (source_)), this);
    }
}

void ModTile::mouseUp (const juce::MouseEvent& e)
{
    if (! draggingTile_ && e.mouseWasClicked() && onClicked)
        onClicked (source_);
    draggingTile_ = false;
}

//==============================================================================
ModStrip::ModStrip (VibeSynthAudioProcessor& processor) : processor_ (processor)
{
    for (int i = 0; i < kNumModSources; ++i)
    {
        const auto src = static_cast<ModSource> (i);
        tiles_[(size_t) i] = std::make_unique<ModTile> (src, processor_);
        tiles_[(size_t) i]->onClicked = [this] (ModSource s)
        {
            if (onTileClicked)
                onTileClicked (s);
        };
        addAndMakeVisible (*tiles_[(size_t) i]);
    }
}

void ModStrip::showPatch (const Patch& patch)
{
    for (auto& t : tiles_)
        t->refreshFromPatch (patch);
    repaint();
}

void ModStrip::tick()
{
    const auto& ov = processor_.nodeOverrides();
    const bool active = ov.liveVoiceActive.load (std::memory_order_relaxed);
    for (int i = 0; i < kNumModSources; ++i)
    {
        const float ph = ov.liveModPhase[i].load (std::memory_order_relaxed);
        const float val = ov.liveModValue[i].load (std::memory_order_relaxed);
        tiles_[(size_t) i]->setPlayhead (ph, val, active);
    }
}

void ModStrip::setSelectedSource (std::optional<ModSource> src)
{
    for (int i = 0; i < kNumModSources; ++i)
        tiles_[(size_t) i]->setSelected (src.has_value() && static_cast<int> (*src) == i);
}

void ModStrip::paint (juce::Graphics& g)
{
    using namespace Theme;
    const auto r = getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (r, radius);
    g.setColour (border);
    g.drawRoundedRectangle (r.reduced (0.5f), radius, hairline);

    g.setColour (dimText);
    g.setFont (font (captionSize, Weight::SemiBold).withExtraKerningFactor (0.12f));
    g.drawText ("MOD", labelArea_, juce::Justification::centred);
}

void ModStrip::resized()
{
    using namespace Theme;
    auto r = getLocalBounds().reduced (6, 6);
    labelArea_ = r.removeFromLeft (34);
    r.removeFromLeft (4);
    const int gap = 6;
    const int tileW = (r.getWidth() - gap * (kNumModSources - 1)) / kNumModSources;
    for (int i = 0; i < kNumModSources; ++i)
    {
        tiles_[(size_t) i]->setBounds (r.removeFromLeft (i < kNumModSources - 1 ? tileW : r.getWidth()));
        if (i < kNumModSources - 1)
            r.removeFromLeft (gap);
    }
}

} // namespace vibesynth::ui
