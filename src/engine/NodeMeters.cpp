// M9: module names for the node visualizer (see NodeMeters.h).

#include "engine/NodeMeters.h"

#include <cctype>

namespace vibesynth {

namespace {

// ModuleId order (Lexicon.h).
constexpr const char* kModuleNames[kNumModules] = {
    "osc_sine", "osc_saw", "osc_tri", "osc_square", "osc_supersaw",
    "osc_wt", "osc_fm", "osc_sync",
    "flt_lp", "flt_hp", "flt_bp", "flt_ladder",
    "flt_formant", "flt_notch", "flt_peak", "flt_tilt",
    "env_adsr", "env_follow", "mod_lfo",
    "fx_fold", "fx_sat", "fx_shape", "fx_crush", "fx_comb", "fx_ott",
    "phys_resonator", "phys_string",
    "chaos_lorenz", "chaos_duffing",
    "util_safe_fb", "util_xover", "util_bytebeat",
    "bus_sat", "bus_mono_low", "bus_chorus", "bus_reverb",
};
static_assert (static_cast<int> (ModuleId::BusReverb) == kNumModules - 1,
               "kModuleNames must list every ModuleId");
// Spot checks that the table follows the enum order.
static_assert (static_cast<int> (ModuleId::FxFold) == 19 && static_cast<int> (ModuleId::FxCrush) == 22
                   && static_cast<int> (ModuleId::UtilBytebeat) == 31 && static_cast<int> (ModuleId::BusSat) == 32,
               "kModuleNames is in ModuleId order");

struct Alias
{
    const char* name;
    ModuleId module;
};

constexpr Alias kAliases[] = {
    { "util_xover_lo", ModuleId::UtilXover },
    { "util_xover_hi", ModuleId::UtilXover },
    { "bus_mono_low_r", ModuleId::BusMonoLow },
    { "bus_chorus_r", ModuleId::BusChorus },
    { "bus_reverb_r", ModuleId::BusReverb },
};

bool sameIgnoringCase (const std::string& a, const char* b) noexcept
{
    size_t i = 0;
    for (; i < a.size() && b[i] != 0; ++i)
        if (std::tolower ((unsigned char) a[i]) != std::tolower ((unsigned char) b[i]))
            return false;
    return i == a.size() && b[i] == 0;
}

} // namespace

const char* moduleShortName (ModuleId module) noexcept
{
    const int m = static_cast<int> (module);
    return m >= 0 && m < kNumModules ? kModuleNames[m] : "";
}

bool isStatelessModule (ModuleId module) noexcept
{
    return module == ModuleId::FxFold || module == ModuleId::FxSat || module == ModuleId::FxShape
        || module == ModuleId::UtilBytebeat || module == ModuleId::BusSat;
}

bool moduleFromName (const std::string& name, ModuleId& module) noexcept
{
    for (int m = 0; m < kNumModules; ++m)
        if (sameIgnoringCase (name, kModuleNames[m]))
        {
            module = static_cast<ModuleId> (m);
            return true;
        }
    for (const auto& a : kAliases)
        if (sameIgnoringCase (name, a.name))
        {
            module = a.module;
            return true;
        }
    return false;
}

} // namespace vibesynth
