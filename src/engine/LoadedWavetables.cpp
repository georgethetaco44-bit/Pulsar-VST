#include "engine/LoadedWavetables.h"

#include <memory>
#include <vector>

namespace vibesynth {

namespace {

std::atomic<const LoadedWavetable*> gTables[kMaxLoadedWavetables] = {};
std::vector<std::unique_ptr<LoadedWavetable>> gOwnedTables;
std::atomic<int> gNumLoaded { 0 };

} // namespace

const LoadedWavetable* getLoadedWavetable (int index) noexcept
{
    const int slot = index - 8;
    if (slot < 0 || slot >= kMaxLoadedWavetables)
        return nullptr;
    return gTables[slot].load (std::memory_order_acquire);
}

void setLoadedWavetable (int slot, std::unique_ptr<LoadedWavetable> wt) noexcept
{
    if (slot < 0 || slot >= kMaxLoadedWavetables || wt == nullptr)
        return;
    const LoadedWavetable* ptr = wt.get();
    gOwnedTables.push_back (std::move (wt));
    gTables[slot].store (ptr, std::memory_order_release);
    int cur = gNumLoaded.load (std::memory_order_relaxed);
    if (slot + 1 > cur)
        gNumLoaded.store (slot + 1, std::memory_order_release);
}

int getNumLoadedWavetables() noexcept
{
    return gNumLoaded.load (std::memory_order_acquire);
}

} // namespace vibesynth
