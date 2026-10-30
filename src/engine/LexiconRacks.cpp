// M4 — LexiconRacks.cpp: Utilities3/Utilities4 out-of-line method bodies.
// (Rack init/reset/metering live in Racks.cpp / Racks2.cpp.)
//
// Real-time rules (SPEC §11): the per-sample paths below perform no
// allocation, locks, logging, I/O, exceptions or std::function; NaN/Inf
// checks use bit-pattern tests. All Init()/buffer sizing happens in
// init(), called off the audio thread from prepareToPlay.

#include "engine/Lexicon.h"
#include "engine/MasterRack.h"

#include <cmath>

namespace vibesynth {

// ---------------------------------------------------------------------------
// Utilities3 method bodies (CombState, SafeFbState).

float CombState::process (float in, double hz, double feedback,
                          double sampleRate) noexcept
{
    if (buf == nullptr || capacity < 4)
        return in;
    const double h = (hz < 10.0 || ! isFiniteBitsD (hz)) ? 10.0 : hz;
    double delay = sampleRate / h;
    const double maxDelay = static_cast<double> (capacity - 2);
    if (delay > maxDelay)
        delay = maxDelay;
    const int i0 = static_cast<int> (delay);
    const double frac = delay - static_cast<double> (i0);
    int r0 = writePos - i0;
    if (r0 < 0)
        r0 += capacity;
    int r1 = r0 - 1;
    if (r1 < 0)
        r1 += capacity;
    const float a = buf[r0];
    const float b = buf[r1];
    const float delayed = a + static_cast<float> (frac) * (b - a);
    const double fb = feedback < 0.0 ? 0.0 : (feedback > 0.95 ? 0.95 : feedback);
    buf[writePos] = in + delayed * static_cast<float> (fb);
    writePos += 1;
    if (writePos >= capacity)
        writePos = 0;
    return delayed;
}

float SafeFbState::process (float x, double sampleRate) noexcept
{
    if (! isFiniteBitsD (static_cast<double> (x)))
        x = 0.0f;
    const double r = 1.0 - 2.0 * 3.14159265358979323846 * 20.0 / sampleRate;
    const double rc = r < 0.0 ? 0.0 : (r > 0.9999 ? 0.9999 : r);
    const double xd = static_cast<double> (x);
    const double blocked = xd - dcPrevIn + rc * dcPrevOut;
    dcPrevIn = xd;
    dcPrevOut = blocked;
    double delta = blocked - last;
    if (delta > 0.25)
        delta = 0.25;
    else if (delta < -0.25)
        delta = -0.25;
    last = fastTanh (last + delta);
    return sanitizeFloat (static_cast<float> (last));
}

// Utilities4 XoverState method bodies.

void XoverState::init (double sampleRate) noexcept
{
    constexpr double kPi = 3.14159265358979323846;
    const double w0 = 2.0 * kPi * kFreq / sampleRate;
    const double cw = std::cos (w0);
    const double sw = std::sin (w0);
    const double Q = 0.7071067811865476;
    const double alpha = sw / (2.0 * Q);
    {
        const double b0 = (1.0 - cw) * 0.5;
        const double b1 = 1.0 - cw;
        const double b2 = b0;
        const double a0 = 1.0 + alpha;
        const double a1 = -2.0 * cw;
        const double a2 = 1.0 - alpha;
        XoverBand* stages[2] = { &lo1, &lo2 };
        for (int i = 0; i < 2; ++i)
        {
            stages[i]->b0 = b0 / a0; stages[i]->b1 = b1 / a0;
            stages[i]->b2 = b2 / a0;
            stages[i]->a1 = a1 / a0; stages[i]->a2 = a2 / a0;
        }
    }
    {
        const double b0 = (1.0 + cw) * 0.5;
        const double b1 = -(1.0 + cw);
        const double b2 = b0;
        const double a0 = 1.0 + alpha;
        const double a1 = -2.0 * cw;
        const double a2 = 1.0 - alpha;
        XoverBand* stages[2] = { &hi1, &hi2 };
        for (int i = 0; i < 2; ++i)
        {
            stages[i]->b0 = b0 / a0; stages[i]->b1 = b1 / a0;
            stages[i]->b2 = b2 / a0;
            stages[i]->a1 = a1 / a0; stages[i]->a2 = a2 / a0;
        }
    }
}

void XoverState::reset() noexcept
{
    lo1.reset(); lo2.reset(); hi1.reset(); hi2.reset();
    lastHi = 0.0f;
}

float XoverState::processLo (float in) noexcept
{
    const float hi = hi2.process (hi1.process (in));
    const float lo = lo2.process (lo1.process (in));
    lastHi = sanitizeFloat (hi);
    return sanitizeFloat (lo);
}

} // namespace vibesynth
