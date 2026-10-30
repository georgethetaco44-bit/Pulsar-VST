// M4 — LexiconArgs.h: internal helpers shared by the LexiconFnN.cpp
// wrappers (rack lookup from EEL2's custom-this, safe argument reads).
// Internal to the engine: only the LexiconFnN.cpp files include this.
#pragma once

// EEL2 headers first (they declare EEL_F/INT_PTR); ours after.
#include "eel2/ns-eel.h"
#include "eel2/ns-eel-addfuncs.h"

#include "engine/Lexicon.h"
#include "engine/MasterRack.h"

namespace vibesynth {

// Rack of the calling VM; nullptr if the VM has none of that type (bus_*
// wrappers return 0 on a voice VM). The master VM has both.
inline VoiceRack* voiceRackOf (void* ctx) noexcept
{
    auto* rc = static_cast<VmRackContext*> (ctx);
    return (rc != nullptr) ? rc->voice : nullptr;
}

inline MasterRack* masterRackOf (void* ctx) noexcept
{
    auto* rc = static_cast<VmRackContext*> (ctx);
    return (rc != nullptr) ? rc->master : nullptr;
}

// EEL passes EEL_F**; out-of-range reads return the default.
inline double argD (EEL_F** parms, INT_PTR np, int i, double dflt) noexcept
{
    if (parms == nullptr || i < 0 || i >= np || parms[i] == nullptr)
        return dflt;
    return static_cast<double> (*parms[i]);
}

} // namespace vibesynth
