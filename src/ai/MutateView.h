#pragma once

// M11: what the audition strip shows (SPEC §9.7). The processor keeps one
// (message thread) and the editor draws it; the snapshot tool fakes one.
// JUCE-free plain data.

#include <array>
#include <string>

namespace vibesynth {

struct MutateSlotView
{
    enum class State { Empty, Waiting, Testing, Ready, Failed, Cancelled };
    State state = State::Empty;
    std::string direction;  // "Darker"
    std::string name;       // Ready: the patch's name
    std::string text;       // one line: "Waiting for model", "Testing (retry 1/2)", the error
    std::string detail;     // Failed: the full reason (tooltip)

    bool operator== (const MutateSlotView& o) const
    {
        return state == o.state && direction == o.direction && name == o.name && text == o.text
            && detail == o.detail;
    }
    bool operator!= (const MutateSlotView& o) const { return ! (*this == o); }
};

struct MutateView
{
    static constexpr int kSlots = 5;

    bool open = false;       // the strip is shown (a session exists)
    bool running = false;    // requests or tests still in flight
    int auditioning = -1;    // the slot playing; -1 = the original
    int numSlots = kSlots;   // M14b: number of active variation slots (1..kSlots)
    std::string baseName;    // the patch the variations come from
    bool takes = false;      // M14e: the cards are Generate takes, not variations
    double startedMs = 0.0;  // juce::Time::getMillisecondCounterHiRes() at Mutate
    std::array<MutateSlotView, kSlots> slots;

    int activeSlots() const noexcept
    {
        if (numSlots > 0 && numSlots <= kSlots)
            return numSlots;
        int n = 0;
        while (n < kSlots && slots[(size_t) n].state != MutateSlotView::State::Empty)
            ++n;
        return n > 0 ? n : kSlots;
    }

    bool operator== (const MutateView& o) const
    {
        return open == o.open && running == o.running && auditioning == o.auditioning && numSlots == o.numSlots
            && baseName == o.baseName && takes == o.takes && slots == o.slots;  // startedMs changes only with the slots
    }
    bool operator!= (const MutateView& o) const { return ! (*this == o); }
};

} // namespace vibesynth
