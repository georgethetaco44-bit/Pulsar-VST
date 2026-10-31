#pragma once

// M8 smoketests (SPEC §6): the api_reference.md <-> lexicon check, the
// lexicon lint, the response parser, the few-shot examples through the
// validator, the prompt builder, the client's error mapping and cancel,
// and the self-heal loop on canned replies. All offline. See SmokeM8.cpp.

namespace vibesynth_cli {

bool smokeApiReference();
bool smokeLint();
bool smokeResponseParser();
bool smokePromptExamples();
bool smokePromptBuilder();
bool smokeClient();
bool smokeSelfHeal();

} // namespace vibesynth_cli
