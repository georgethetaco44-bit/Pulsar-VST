// M9 node visualizer and output meter. See NodeView.h.

#include "ui/NodeView.h"

#include <cmath>

namespace vibesynth::ui {

namespace {
constexpr float kFloorDb = -48.0f;
constexpr double kDecaySeconds = 0.150;   // visual decay time constant
constexpr float kVisible = 0.01f;         // below this a node draws no glow
constexpr float kPillPad = 7.0f;       // label to pill edge
constexpr float kPillGap = 12.0f;      // least space between columns (the arrows)
constexpr float kMaxPillFont = 11.0f;  // labels step down from here to fit
constexpr float kMinPillFont = 8.0f;
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
                "They glow with their live level. Shapers without a slot are numbered by call order "
                "(fx_sat #1 is the second fx_sat in a sample). Lines: the model's own description of the "
                "signal flow.");
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

std::vector<std::string> NodeView::placementProblems() const
{
    std::vector<std::string> problems;
    const auto f = Theme::mono (pillFontSize);
    const auto bounds = getLocalBounds().toFloat();
    for (size_t i = 0; i < boxes.size(); ++i)
    {
        const juce::String name (NodeGraphLayout::label (layout.nodes[i]));
        const float need = juce::GlyphArrangement::getStringWidth (f, name);
        if (boxes[i].getWidth() - 6.0f < need)
            problems.push_back (name.toStdString() + " is cut off");
        if (! bounds.contains (boxes[i]))
            problems.push_back (name.toStdString() + " is outside the view");
        else
        {
            // Inside its region, below the VOICE / MASTER label.
            auto region = (layout.nodes[i].master ? masterArea : voiceArea).toFloat();
            region.setTop ((float) graphArea.getY() + (float) Theme::grid * 3.0f);
            region.setBottom ((float) graphArea.getBottom());
            if (! region.expanded (1.0f).contains (boxes[i]))
                problems.push_back (name.toStdString() + " leaves its region");
        }
        for (size_t j = i + 1; j < boxes.size(); ++j)
            if (boxes[i].intersects (boxes[j]))
                problems.push_back (name.toStdString() + " overlaps "
                                    + NodeGraphLayout::label (layout.nodes[j]));
    }
    return problems;
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

    // Pills are as wide as their label. The font steps down from 11 pt
    // only when the two regions can't hold the full names.
    const float total = (float) (graphArea.getWidth() - grid * 2);
    float needs[2] = {};
    std::vector<float> widths (layout.nodes.size(), 0.0f);
    std::vector<std::vector<float>> colW[2];
    auto measure = [&] (float size)
    {
        const auto f = mono (size);
        for (size_t i = 0; i < layout.nodes.size(); ++i)
            widths[i] = juce::GlyphArrangement::getStringWidth (f, juce::String (NodeGraphLayout::label (layout.nodes[i])))
                      + kPillPad * 2.0f;
        for (int region = 0; region < 2; ++region)
        {
            std::vector<float> layerW ((size_t) juce::jmax (0, layout.layers[region]), 0.0f);
            float widest = 0.0f;
            for (size_t i = 0; i < layout.nodes.size(); ++i)
            {
                const auto& n = layout.nodes[i];
                if ((n.master ? 1 : 0) != region)
                    continue;
                widest = juce::jmax (widest, widths[i]);
                if (! n.loose && n.layer < (int) layerW.size())
                    layerW[(size_t) n.layer] = juce::jmax (layerW[(size_t) n.layer], widths[i]);
            }
            float layered = 0.0f;
            for (const float w : layerW)
                layered += w;
            layered += kPillGap * (float) juce::jmax (0, (int) layerW.size() - 1);
            needs[region] = juce::jmax (layered, widest);
            colW[region] = { layerW };
        }
    };
    pillFontSize = kMaxPillFont;
    measure (pillFontSize);
    while (needs[0] + needs[1] > total && pillFontSize > kMinPillFont)
    {
        pillFontSize -= 0.5f;
        measure (pillFontSize);
    }

    const float vw = (float) juce::jmax (1, layout.layers[0]);
    const float mw = (float) juce::jmax (1, layout.layers[1]);
    float share = masterCount == 0 ? 0.76f : juce::jlimit (0.42f, 0.78f, vw / (vw + mw));
    if (voiceCount == 0)
        share = 0.3f;
    float voiceW = total * share;
    voiceW = juce::jmax (needs[0], juce::jmin (voiceW, total - needs[1]));
    auto area = graphArea;
    voiceArea = area.removeFromLeft (juce::roundToInt (voiceW));
    dividerX = (float) voiceArea.getRight() + (float) grid;
    area.removeFromLeft (grid * 2);
    masterArea = area;

    for (int region = 0; region < 2; ++region)
    {
        auto r = (region == 0 ? voiceArea : masterArea);
        r.removeFromTop (grid * 3);  // region label
        const int numLayers = layout.layers[region];
        const int numRows = layout.rows[region];

        // Loose nodes: rows of pills at the bottom, wrapped by width.
        std::vector<std::vector<size_t>> looseRows;
        {
            float used = 0.0f;
            for (size_t i = 0; i < layout.nodes.size(); ++i)
            {
                const auto& n = layout.nodes[i];
                if ((n.master ? 1 : 0) != region || ! n.loose)
                    continue;
                if (looseRows.empty() || used + (float) grid + widths[i] > (float) r.getWidth())
                {
                    looseRows.emplace_back();
                    used = -(float) grid;
                }
                looseRows.back().push_back (i);
                used += (float) grid + widths[i];
            }
        }
        const int numLooseRows = (int) looseRows.size();
        // Rows are kPillH + 8 apart, closer (and the pills lower) when that
        // wouldn't fit: a master with many bus_sat calls stacks up.
        const int looseAvail = numLayers > 0 ? r.getHeight() / 2 : r.getHeight();
        int pitch = (int) kPillH + grid;
        if (numLooseRows > 0 && (numLooseRows - 1) * pitch + (int) kPillH > looseAvail)
            pitch = juce::jmax (18, (looseAvail - (int) kPillH) / juce::jmax (1, numLooseRows - 1));
        const float loosePillH = juce::jmin (kPillH, (float) pitch - 4.0f);
        // Below the layered nodes, or centred when there are none.
        const int looseH = numLooseRows > 0 ? (numLooseRows - 1) * pitch + (int) loosePillH : 0;
        auto looseArea = numLayers > 0 ? r.removeFromBottom (looseH) : r.withSizeKeepingCentre (r.getWidth(), looseH);
        if (numLooseRows > 0 && numLayers > 0)
            r.removeFromBottom (grid);

        for (int row = 0; row < numLooseRows; ++row)
        {
            float rowW = (float) grid * (float) ((int) looseRows[(size_t) row].size() - 1);
            for (const size_t i : looseRows[(size_t) row])
                rowW += widths[i];
            float x = (float) looseArea.getX() + ((float) looseArea.getWidth() - rowW) * 0.5f;
            for (const size_t i : looseRows[(size_t) row])
            {
                boxes[i] = { x, (float) looseArea.getY() + (float) (row * pitch), widths[i], loosePillH };
                x += widths[i] + (float) grid;
            }
        }

        // Layered nodes: a column per layer as wide as its widest pill, the
        // spare width shared out as gaps (a single layer is centred).
        const auto& layerW = colW[region].empty() ? std::vector<float>() : colW[region][0];
        float layerSum = 0.0f;
        for (const float w : layerW)
            layerSum += w;
        const float gap = numLayers > 1 ? juce::jmax (kPillGap, ((float) r.getWidth() - layerSum) / (float) (numLayers - 1))
                                        : 0.0f;
        std::vector<float> colX ((size_t) juce::jmax (0, numLayers), 0.0f);
        float x = numLayers > 1 ? (float) r.getX() : (float) r.getCentreX() - (layerSum * 0.5f);
        for (int l = 0; l < numLayers; ++l)
        {
            colX[(size_t) l] = x;
            x += layerW[(size_t) l] + gap;
        }
        const float rowH = numRows > 0 ? (float) r.getHeight() / (float) numRows : 0.0f;
        const float pillH = juce::jmin (kPillH, rowH - (float) grid);

        for (size_t i = 0; i < layout.nodes.size(); ++i)
        {
            const auto& n = layout.nodes[i];
            if ((n.master ? 1 : 0) != region || n.loose)
                continue;
            // Centre the layer's rows vertically.
            int inLayer = 0;
            for (const auto& o : layout.nodes)
                if (o.master == n.master && ! o.loose && o.layer == n.layer)
                    ++inLayer;
            const float layerTop = (float) r.getY() + ((float) numRows - (float) inLayer) * rowH * 0.5f;
            const float cx = colX[(size_t) n.layer] + layerW[(size_t) n.layer] * 0.5f;
            const float cy = layerTop + rowH * ((float) n.row + 0.5f);
            boxes[i] = juce::Rectangle<float> (widths[i], pillH).withCentre ({ cx, cy });
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
    const auto labelFont = mono (pillFontSize);
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
        g.drawFittedText (juce::String (NodeGraphLayout::label (layout.nodes[i])), box.toNearestInt().reduced (3, 0),
                          juce::Justification::centred, 1, 0.8f);
    }
}

//==============================================================================
OutputMeter::OutputMeter()
{
    setTooltip ("Output peak, left and right (-48 to 0 dBFS). The bright mark holds the recent peak; "
                "amber at -0.5 dBFS and above.");
}

namespace {
constexpr float kHotPeak = 0.944f;       // -0.5 dBFS
constexpr double kHoldMs = 1500.0;       // peak-hold time before the mark falls
constexpr float kTickDb[] = { -36.0f, -24.0f, -12.0f, -6.0f };
} // namespace

void OutputMeter::tick (float peakL, float peakR, double nowMs)
{
    const float decay = lastTickMs > 0.0 ? decayFactor (nowMs - lastTickMs) : 0.0f;
    lastTickMs = nowMs;
    bool changed = false;
    const float in[2] = { peakL, peakR };
    for (int c = 0; c < 2; ++c)
    {
        const float fresh = meterLevel (in[c]);
        float level = juce::jmax (fresh, levels[c] * decay);
        if (level < kVisible)
            level = 0.0f;
        changed = changed || std::abs (level - levels[c]) > 1.0e-4f;
        levels[c] = level;

        // Peak hold: raised at once, kept for kHoldMs, then falls with the
        // same decay as the bar.
        float hold = holds[c];
        if (fresh > 0.0f && fresh >= hold)
        {
            hold = fresh;
            holdSetMs[c] = nowMs;
            hot[c] = in[c] > kHotPeak;
        }
        else if (nowMs - holdSetMs[c] > kHoldMs)
        {
            hold *= decay;
            if (hold < kVisible)
            {
                hold = 0.0f;
                hot[c] = false;
            }
        }
        changed = changed || std::abs (hold - holds[c]) > 1.0e-4f;
        holds[c] = hold;
    }
    if (changed)
        repaint();
}

void OutputMeter::paint (juce::Graphics& g)
{
    using namespace Theme;
    const auto frame = getLocalBounds().toFloat();
    g.setColour (background);
    g.fillRoundedRectangle (frame, 4.0f);
    g.setColour (border.brighter (0.25f));
    g.drawRoundedRectangle (frame.reduced (0.5f), 4.0f, hairline);

    auto inner = frame.reduced (5.0f, 4.0f);
    auto captions = inner.removeFromLeft (10.0f);
    inner.removeFromLeft (3.0f);
    const float barH = (inner.getHeight() - 2.0f) * 0.5f;
    const juce::Rectangle<float> tracks[2] = { inner.withHeight (barH),
                                               inner.withTrimmedTop (barH + 2.0f).withHeight (barH) };

    g.setFont (font (9.0f, Weight::SemiBold));
    for (int c = 0; c < 2; ++c)
    {
        const auto& track = tracks[c];
        g.setColour (dimText);
        g.drawText (c == 0 ? "L" : "R", captions.withY (track.getY() - 1.0f).withHeight (barH + 2.0f),
                    juce::Justification::centred);

        // A dim full-length track (the -inf side of the scale), then the level.
        g.setColour (border);
        g.fillRoundedRectangle (track, 1.5f);
        if (levels[c] > 0.0f)
        {
            g.setColour (hot[c] && levels[c] > 0.9f ? warning : accent);
            g.fillRoundedRectangle (track.withWidth (track.getWidth() * levels[c]), 1.5f);
        }
        // Scale: ticks at -36/-24/-12/-6 dBFS; 0 dBFS is the right end.
        g.setColour (background.withAlpha (0.7f));
        for (const float db : kTickDb)
            g.fillRect (track.getX() + track.getWidth() * (db - kFloorDb) / -kFloorDb - 0.5f, track.getY(), 1.0f,
                        track.getHeight());
        // The held peak.
        if (holds[c] > 0.0f)
        {
            g.setColour (hot[c] ? warning : text);
            g.fillRect (track.getX() + juce::jmax (0.0f, track.getWidth() * holds[c] - 2.0f), track.getY() - 0.5f, 2.0f,
                        track.getHeight() + 1.0f);
        }
    }
}

} // namespace vibesynth::ui
