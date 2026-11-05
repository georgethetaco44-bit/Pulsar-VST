#include "engine/LoadedWavetables.h"

#include <memory>

namespace vibesynth {

namespace {

std::atomic<const LoadedWavetable*> gTables[kMaxLoadedWavetables] = {};
std::unique_ptr<LoadedWavetable> gOwnedTables[kMaxLoadedWavetables];  // kept until exit: voices hold pointers
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
    if (slot < 0 || slot >= kMaxLoadedWavetables || wt == nullptr || gOwnedTables[slot] != nullptr)
        return;
    const LoadedWavetable* ptr = wt.get();
    gOwnedTables[slot] = std::move (wt);
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
