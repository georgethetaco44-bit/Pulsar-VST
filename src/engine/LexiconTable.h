#pragma once

// M8: the registered lexicon as data (SPEC §5.2, §6.4). One table drives
// registerLexiconFunctions(), the lexicon lint (patch/LexiconLint.h) and
// the smoketest that checks resources/prompts/api_reference.md against
// what is really registered, so the three can't drift apart.
//
// JUCE-free and EEL2-free; LexiconReg.cpp pairs each entry with its
// wrapper function.

namespace vibesynth {

enum class LexiconScope
{
    Both,   // voice and master (the master has its own VoiceRack, M6)
    Master  // bus_*: master only (they return 0 in a voice VM)
};

struct LexiconEntry
{
    const char* name;
    const char* params;   // "slot, hz": the signature as documented
    int numArgs;          // exact argument count
    int numSlots;         // > 0: the first argument is a slot, 0..numSlots-1; 0: no slot
    LexiconScope scope;
};

int numLexiconEntries() noexcept;
const LexiconEntry& lexiconEntry (int index) noexcept;
// Case-insensitive, as EEL2 resolves names. Null if not registered.
const LexiconEntry* findLexiconEntry (const char* name) noexcept;

} // namespace vibesynth
