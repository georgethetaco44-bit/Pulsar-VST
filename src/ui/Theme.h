#pragma once

// M9 look (SPEC §13 M9: dark, one neon accent). Every colour, size and
// font the editor uses comes from here; PulsarLookAndFeel maps them onto
// JUCE's colour IDs. Rules: flat panels, glow only on live things (meters,
// nodes, the generating state), no decorative gradients, an 8 px grid,
// cyan is the only accent and amber is only for warnings and errors.

#include <juce_gui_basics/juce_gui_basics.h>

namespace vibesynth::ui {

namespace Theme {

// Palette.
inline const juce::Colour background { 0xff0b0d12 };  // near-black
inline const juce::Colour panel      { 0xff141821 };  // raised panels
inline const juce::Colour control    { 0xff1a1f2a };  // controls on panels (one step up)
inline const juce::Colour border     { 0xff232a36 };  // hairlines
inline const juce::Colour text       { 0xffe6e9ef };
inline const juce::Colour dimText    { 0xff8a93a6 };
inline const juce::Colour accent     { 0xff3df5ff };  // active state, arcs, glow, Generate
inline const juce::Colour warning    { 0xffffb347 };  // warnings and errors only

// Geometry (logical pixels at 1x; the editor scales as a whole).
constexpr int grid = 8;
constexpr int baseWidth = 960;
constexpr int baseHeight = 640;
constexpr float radius = 6.0f;       // panels and controls
constexpr float hairline = 1.0f;

// Type: Inter (UI) and JetBrains Mono (code), both OFL-1.1, embedded.
enum class Weight { Regular, Medium, SemiBold };
juce::Font font (float height, Weight weight = Weight::Regular);
juce::Font mono (float height);

// Typical sizes.
constexpr float textSize = 14.0f;
constexpr float smallSize = 12.0f;
constexpr float captionSize = 11.0f;

} // namespace Theme

} // namespace vibesynth::ui
