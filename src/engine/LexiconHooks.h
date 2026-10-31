#pragma once

// M10: the shared hook layer between EEL2 and the lexicon wrappers (SPEC
// §9.6). Every registered function's thunk (LexiconReg.cpp) calls
// callHooked() instead of the wrapper when the calling rack is `hooked`
// (it has overrides or is the trace voice). callHooked():
//   1. finds the node: (rack kind, module, slot literal or the stateless
//      call index before this call, 0 for bus_*);
//   2. copies the arguments and applies the node's overrides to the
//      editable ones (primary function only; NodeOverrides.h has the maths);
//   3. bypass: PassArg returns the passed argument, Zero returns 0 (both
//      skip the wrapper but still meter, so stateless call numbering and
//      the touched set are unchanged), ForceArg runs the wrapper with one
//      argument fixed;
//   4. otherwise runs the same wrapper on the effective arguments;
//   5. records the output and the effective arguments into the rack's
//      TraceSink (primary function only).
// Audio thread: no allocation, locks, logging, I/O or exceptions.

#include "eel2/ns-eel.h"
#include "eel2/ns-eel-addfuncs.h"

#include "engine/LexiconTable.h"

namespace vibesynth {

using LexiconWrapperFn = EEL_F (NSEEL_CGEN_CALL*) (void*, INT_PTR, EEL_F**);

EEL_F callHooked (const LexiconEntry& entry, LexiconWrapperFn fn, void* ctx, INT_PTR np, EEL_F** parms) noexcept;

} // namespace vibesynth
