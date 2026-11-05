#pragma once

// Prototype smoketests: stereo voices and their lint and validation, osc_unison,
// mod_curve and mod_curve_note against the formula, wavetable files.

namespace vibesynth_cli {

bool smokePrototype();

// --wavetable-load-test: times the background scan of <app-data>/Wavetables/
// and stopping it mid-load, as a plugin being closed does. Only reads the
// folder. Run it with a temporary HOME that holds the test tables.
int runWavetableLoadTest();

} // namespace vibesynth_cli
