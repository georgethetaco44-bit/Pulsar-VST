// M9 node visualizer and output meter. See NodeView.h.

#include "ui/NodeView.h"

#include <cmath>

namespace vibesynth::ui {

namespace {
constexpr float kFloorDb = -48.0f;
constexpr double kDecaySeconds = 0.150;   // visual decay time constant
constexpr float kVisible = 0.01f;         // below this a node draws no glow
constexpr float kMaxPillW = 132.0f;
constexpr float kPillH = 26.0f;

std::vector<std::pair<std::string, std::string>> parseEdges (const std::string& graphJson)
{
    std::vector<std::pair<std::string, std::string>> edges;
    if (graphJson.empty())
        return edges;
    const auto graph = juce::JSON::parse (juce::String (graphJson));
    if (const auto* list = graph.getProperty ("edges", {}).getArray())
        for (const auto& e : *list)
            if (const auto* pair = e.getArray(); pair != nullptr && pair->size() == 2)
                edges.emplace_back ((*pair)[0].toString().toStdString(), (*pair)[1].toString().toStdString());
    return edges;
}

float decayFactor (double dtMs)
{
    return (float) std::exp (-juce::jlimit (0.0, 500.0, dtMs) / (1000.0 * kDecaySeconds));
}

void drawCaps (juce::Graphics& g, const juce::String& s, juce::Rectangle<int> r, juce::Justification j)
{
    g.setFont (Theme::font (Theme::captionSize, Theme::Weight::SemiBold).withExtraKerningFactor (0.12f));
    g.drawText (s, r, j);
}
} // namespace

float meterLevel (float peak) noexcept
{
    if (! (peak > 1.0e-6f))
        return 0.0f;
    const float db = 20.0f * std::log10 (peak);
    return juce::jlimit (0.0f, 1.0f, (db - kFloorDb) / -kFloorDb);
}

//==============================================================================
NodeView::NodeView()
{
    setTooltip ("Nodes: the module slots the patch's code really runs (found by the offline test). "
                "They glow with their live level. Lines: the model's own description of the signal flow.");
    setInterceptsMouseClicks (true, false);
}

void NodeView::setGraph (const TouchedNodes& nodes, bool isKnown, const std::string& graphJson)
{
    known = isKnown;
    layout = layoutNodeGraph (nodes, parseEdges (graphJson));
    levels.assign (layout.nodes.size(), 0.0f);
    glowing = false;
    place();
    repaint();
}

void NodeView::tick (NodeMeterBank& bank, double nowMs)
{
    const float decay = lastTickMs > 0.0 ? decayFactor (nowMs - lastTickMs) : 0.0f;
    lastTickMs = nowMs;

    // Take every cell, so nothing stale is left for the next patch.
    float taken[2][kNumModules][kMaxSlotsPerModule];
    for (int m = 0; m < kNumModules; ++m)
        for (int s = 0; s < kMaxSlotsPerModule; ++s)
        {
            taken[0][m][s] = NodeMeterBank::take (bank.voice[m][s]);
            taken[1][m][s] = NodeMeterBank::take (bank.master[m][s]);
        }

    bool any = false;
    for (size_t i = 0; i < layout.nodes.size(); ++i)
    {
        const auto& n = layout.nodes[i];
        const float fresh = meterLevel (taken[n.master ? 1 : 0][(int) n.module][n.slot]);
        float level = juce::jmax (fresh, levels[i] * decay);
        if (level < kVisible)
            level = 0.0f;
        levels[i] = level;
        any = any || level > 0.0f;
    }
    // Silent and already drawn silent: nothing to repaint.
    if (any || glowing)
        repaint (graphArea.expanded (Theme::grid * 2));
    glowing = any;
}

void NodeView::resized()
{
    using namespace Theme;
    auto r = getLocalBounds().reduced (grid * 2);
    r.removeFromTop (grid * 3);     // title row
    r.removeFromBottom (grid * 3);  // caption row
    graphArea = r.reduced (0, grid);
    place();
}

void NodeView::place()
{
    using namespace Theme;
    boxes.assign (layout.nodes.size(), {});
    if (graphArea.isEmpty())
        return;

    int voiceCount = 0, masterCount = 0;
    for (const auto& n : layout.nodes)
        (n.master ? masterCount : voiceCount)++;
    const float vw = (float) juce::jmax (1, layout.layers[0]);
    const float mw = (float) juce::jmax (1, layout.layers[1]);
    float share = masterCount == 0 ? 0.76f : juce::jlimit (0.42f, 0.78f, vw / (vw + mw));
    if (voiceCount == 0)
        share = 0.3f;
    auto area = graphArea;
    voiceArea = area.removeFromLeft (juce::roundToInt ((float) area.getWidth() * share));
    dividerX = (float) voiceArea.getRight() + (float) grid;
    area.removeFromLeft (grid * 2);
    masterArea = area;

    for (int region = 0; region < 2; ++region)
    {
        auto r = (region == 0 ? voiceArea : masterArea);
        r.removeFromTop (grid * 3);  // region label
        const int looseCount = layout.loose[region];
        const int numLayers = layout.layers[region];
        const int numRows = layout.rows[region];

        // Loose nodes: rows of pills at the bottom, wrapping as needed.
        const float looseW = juce::jmin (kMaxPillW, (float) r.getWidth());
        const int perRow = juce::jmax (1, (int) ((r.getWidth() + grid) / (int) (looseW * 0.8f + grid)));
        const int looseRows = looseCount > 0 ? (looseCount + perRow - 1) / perRow : 0;
        // Below the layered nodes, or centred when there are none.
        const int looseH = looseRows * (int) (kPillH + grid) - (looseRows > 0 ? grid : 0);
        auto looseArea = numLayers > 0 ? r.removeFromBottom (looseH) : r.withSizeKeepingCentre (r.getWidth(), looseH);
        if (looseRows > 0 && numLayers > 0)
            r.removeFromBottom (grid);

        const float colW = numLayers > 0 ? (float) r.getWidth() / (float) numLayers : 0.0f;
        const float rowH = numRows > 0 ? (float) r.getHeight() / (float) numRows : 0.0f;
        const float pillW = juce::jmin (kMaxPillW, colW - (float) grid * 2.0f);
        const float pillH = juce::jmin (kPillH, rowH - (float) grid);

        int looseIndex = 0;
        for (size_t i = 0; i < layout.nodes.size(); ++i)
        {
            const auto& n = layout.nodes[i];
            if ((n.master ? 1 : 0) != region)
                continue;
            if (n.loose)
            {
                const int row = looseIndex / perRow, col = looseIndex % perRow;
                const int inRow = juce::jmin (perRow, looseCount - row * perRow);
                const float w = juce::jmin (kMaxPillW, ((float) looseArea.getWidth() - (float) grid * (float) (inRow - 1))
                                                           / (float) inRow);
                const float totalW = w * (float) inRow + (float) grid * (float) (inRow - 1);
                const float x0 = (float) looseArea.getX() + ((float) looseArea.getWidth() - totalW) * 0.5f;
                boxes[i] = { x0 + (float) col * (w + (float) grid),
                             (float) looseArea.getY() + (float) row * (kPillH + (float) grid), w, kPillH };
                ++looseIndex;
                continue;
            }
            // Centre the layer's rows vertically.
            int inLayer = 0;
            for (const auto& o : layout.nodes)
                if (o.master == n.master && ! o.loose && o.layer == n.layer)
                    ++inLayer;
            const float layerTop = (float) r.getY() + ((float) numRows - (float) inLayer) * rowH * 0.5f;
            const float cx = (float) r.getX() + colW * ((float) n.layer + 0.5f);
            const float cy = layerTop + rowH * ((float) n.row + 0.5f);
            boxes[i] = juce::Rectangle<float> (pillW, pillH).withCentre ({ cx, cy });
        }
    }
}

void NodeView::paint (juce::Graphics& g)
{
    using namespace Theme;
    const auto bounds = getLocalBounds().toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (border);
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, hairline);

    auto inner = getLocalBounds().reduced (grid * 2);
    auto titleRow = inner.removeFromTop (grid * 3);
    auto captionRow = inner.removeFromBottom (grid * 3);
    g.setColour (dimText);
    drawCaps (g, "SIGNAL", titleRow, juce::Justification::centredLeft);
    if (known)
    {
        g.setFont (font (smallSize));
        const int n = (int) layout.nodes.size();
        g.drawText (juce::String (n) + (n == 1 ? " node" : " nodes"), titleRow, juce::Justification::centredRight);
    }
    g.setColour (dimText.withAlpha (0.8f));
    g.setFont (font (captionSize));
    g.drawText (juce::String::fromUTF8 ("graph from model \xc2\xb7 descriptive"), captionRow,
                juce::Justification::bottomLeft);

    if (! known || layout.nodes.empty())
    {
        g.setColour (dimText);
        g.setFont (font (13.0f));
        g.drawText (known ? "No metered modules in this patch" : juce::String::fromUTF8 ("Reading nodes\xe2\x80\xa6"),
                    graphArea, juce::Justification::centred);
        return;
    }

    // Region labels and the divider.
    g.setColour (dimText);
    drawCaps (g, "VOICE", voiceArea.withHeight (grid * 2), juce::Justification::centredLeft);
    drawCaps (g, "MASTER", masterArea.withHeight (grid * 2), juce::Justification::centredLeft);
    g.setColour (border);
    g.fillRect (dividerX - 0.5f, (float) graphArea.getY(), hairline, (float) graphArea.getHeight());
    if (layout.loose[1] + layout.layers[1] == 0)
    {
        g.setColour (dimText.withAlpha (0.7f));
        g.setFont (font (smallSize));
        g.drawText ("passthrough", masterArea, juce::Justification::centred);
    }

    // Edges: thin, static, descriptive.
    for (const auto& e : layout.edges)
    {
        const auto& a = boxes[(size_t) e.from];
        const auto& b = boxes[(size_t) e.to];
        juce::Path p;
        juce::Point<float> end;
        juce::Point<float> dir;
        if (e.feedback || b.getX() <= a.getRight())
        {
            // Feedback (or same column): loop under the pills.
            const juce::Point<float> s (a.getCentreX(), a.getBottom());
            end = { b.getCentreX(), b.getBottom() };
            const float dip = juce::jmax (s.y, end.y) + (float) grid * 2.5f;
            p.startNewSubPath (s);
            p.cubicTo (s.x, dip, end.x, dip, end.x, end.y + 1.0f);
            dir = { 0.0f, -1.0f };
        }
        else
        {
            const juce::Point<float> s (a.getRight(), a.getCentreY());
            end = { b.getX(), b.getCentreY() };
            const float dx = (end.x - s.x) * 0.5f;
            p.startNewSubPath (s);
            p.cubicTo (s.x + dx, s.y, end.x - dx, end.y, end.x - 1.0f, end.y);
            dir = { 1.0f, 0.0f };
        }
        const auto ink = dimText.withAlpha (e.crossRegion ? 0.28f : 0.45f);
        g.setColour (ink);
        if (e.feedback)
        {
            juce::Path dashed;
            const float dashes[] = { 3.0f, 3.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, p, dashes, 2);
            g.fillPath (dashed);
        }
        else
        {
            g.strokePath (p, juce::PathStrokeType (1.0f));
        }
        juce::Path arrow;
        const juce::Point<float> side (-dir.y, dir.x);
        arrow.addTriangle (end, end - dir * 5.0f + side * 3.0f, end - dir * 5.0f - side * 3.0f);
        g.fillPath (arrow);
    }

    // Nodes, with their glow.
    const auto labelFont = mono (11.0f);
    for (size_t i = 0; i < layout.nodes.size(); ++i)
    {
        const auto& box = boxes[i];
        const float level = levels[i];
        if (level > 0.0f)
        {
            // A pill-shaped halo: stacked translucent outlines, wider and
            // brighter with the level.
            g.setColour (accent.withAlpha (0.06f * level));
            for (int k = 4; k >= 1; --k)
            {
                const float grow = (float) k * (0.75f + 1.25f * level);
                g.fillRoundedRectangle (box.expanded (grow), box.getHeight() * 0.5f + grow);
            }
        }
        g.setColour (control.interpolatedWith (accent, 0.16f * level));
        g.fillRoundedRectangle (box, box.getHeight() * 0.5f);
        g.setColour (level > 0.0f ? accent.withAlpha (0.35f + 0.65f * level) : border.brighter (0.25f));
        g.drawRoundedRectangle (box.reduced (0.5f), box.getHeight() * 0.5f, level > 0.0f ? 1.5f : hairline);
        g.setColour (text.withAlpha (0.78f + 0.22f * level));
        g.setFont (labelFont);
        g.drawFittedText (juce::String (NodeGraphLayout::label (layout.nodes[i])), box.toNearestInt().reduced (6, 0),
                          juce::Justification::centred, 1, 0.8f);
    }
}

//==============================================================================
OutputMeter::OutputMeter()
{
    setTooltip ("Output level, left and right (-48 to 0 dBFS; amber above -0.5 dBFS)");
}

void OutputMeter::tick (float peakL, float peakR, double nowMs)
{
    const float decay = lastTickMs > 0.0 ? decayFactor (nowMs - lastTickMs) : 0.0f;
    lastTickMs = nowMs;
    bool changed = false;
    const float in[2] = { peakL, peakR };
    for (int c = 0; c < 2; ++c)
    {
        float level = juce::jmax (meterLevel (in[c]), levels[c] * decay);
        if (level < kVisible)
            level = 0.0f;
        changed = changed || std::abs (level - levels[c]) > 1.0e-4f;
        levels[c] = level;
        peaks[c] = juce::jmax (in[c], peaks[c] * decay);
    }
    if (changed)
        repaint();
}

void OutputMeter::paint (juce::Graphics& g)
{
    using namespace Theme;
    auto r = getLocalBounds().toFloat();
    const float barH = 4.0f;
    const float gap = 4.0f;
    float y = r.getCentreY() - barH - gap * 0.5f;
    for (int c = 0; c < 2; ++c)
    {
        const auto track = juce::Rectangle<float> (r.getX(), y, r.getWidth(), barH);
        g.setColour (border);
        g.fillRoundedRectangle (track, 2.0f);
        if (levels[c] > 0.0f)
        {
            const bool hot = peaks[c] > 0.944f; // -0.5 dBFS
            g.setColour (hot ? warning : accent);
            g.fillRoundedRectangle (track.withWidth (track.getWidth() * levels[c]), 2.0f);
        }
        y += barH + gap;
    }
}

} // namespace vibesynth::ui
