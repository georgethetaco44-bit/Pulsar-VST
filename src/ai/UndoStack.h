#pragma once

// M11 Undo (SPEC §9.7): every swap (generate, Mutate Keep, Change this
// node, preset or file load) pushes what was playing before it; Undo pops
// it back, compiled with its stored trim (no offline test: it passed when
// it was first loaded), with the exact macro values it had.
//
// Message thread only (the processor owns one). JUCE-free so
// vibesynth_cli's smoketest can test it.

#include <array>
#include <cstring>
#include <deque>
#include <string>

#include "engine/NodeMeters.h"
#include "patch/Patch.h"

namespace vibesynth {

struct PatchSnapshot
{
    static constexpr int kNumMacros = 8;

    Patch patch;                           // code, labels, defaults, trim, tweaks, macro maps
    std::array<float, kNumMacros> macros {}; // the macro parameters' values
    std::string source;                    // the editor's highlight: "builtin:3", a file path, ...
    int program = -1;                      // the host program index then
    TouchedNodes nodes;                    // the node view's list, if known
    bool nodesKnown = false;
    // M10's tweaks and macro maps are part of `patch` (Patch::tweaks,
    // macroMaps), so they need no field of their own.

    // Bit-exact (patch numbers and macro floats), for the smoketest.
    bool sameAs (const PatchSnapshot& o) const noexcept
    {
        return patch == o.patch && std::memcmp (macros.data(), o.macros.data(), sizeof (float) * kNumMacros) == 0
            && source == o.source && program == o.program && nodes == o.nodes && nodesKnown == o.nodesKnown;
    }
};

class UndoStack
{
public:
    static constexpr size_t kDepth = 20;

    // The oldest entry falls off past kDepth.
    void push (PatchSnapshot s)
    {
        entries.push_back (std::move (s));
        while (entries.size() > kDepth)
            entries.pop_front();
    }

    // False when empty.
    bool pop (PatchSnapshot& out)
    {
        if (entries.empty())
            return false;
        out = std::move (entries.back());
        entries.pop_back();
        return true;
    }

    bool canUndo() const noexcept { return ! entries.empty(); }
    size_t size() const noexcept { return entries.size(); }
    const PatchSnapshot* top() const noexcept { return entries.empty() ? nullptr : &entries.back(); }
    void clear() noexcept { entries.clear(); }

private:
    std::deque<PatchSnapshot> entries;
};

} // namespace vibesynth
