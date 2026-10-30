// M4 — ChaosMath.h: Lorenz + Duffing state structs and step functions.
//
// SPEC §5.2 MVP: chaos_lorenz(slot, rate, axis), chaos_duffing(slot, hz, chaos).
// Header-only, no allocation, audio-thread safe (plain POD stepping).
//
// Real-time rules (SPEC §11): no allocation/locks/logging/I/O, no exceptions.
// NaN/Inf guards use bit-pattern tests, never std::isnan (see Utilities.h).
#pragma once

namespace vibesynth {

// ---------------------------------------------------------------------------
// Lorenz attractor (classic σ=10, ρ=28, β=8/3 system), RK2 (midpoint).
// The wrapper maps `rate` to a per-sample time advance and splits it into
// at most kMaxSubsteps substeps of ≤ kMaxSubstepDt each (SPEC §4.2.1 budget).
// Divergence protection: if any coordinate exceeds 1e3, return to the
// voice's seeded start point — deterministic, no NaN can propagate.

struct LorenzState
{
    static constexpr int kMaxSubsteps = 4;
    static constexpr double kMaxSubstepDt = 0.005;
    // Dominant orbit frequency ≈ 1.62 cycles per Lorenz time unit
    // (imaginary part of the eigenvalues at C±: ω ≈ 10.2 rad/unit).
    static constexpr double kCyclesPerTimeUnit = 1.62;

    double x = 1.0;
    double y = 1.0;
    double z = 1.0;
    // Per-voice start point (seeded in VoiceRack::init so voices diverge).
    double x0 = 1.0;
    double y0 = 1.0;
    double z0 = 1.0;

    void reset() noexcept
    {
        x = x0;
        y = y0;
        z = z0;
    }

    void step (double dt) noexcept
    {
        constexpr double sigma = 10.0;
        constexpr double rho = 28.0;
        constexpr double beta = 8.0 / 3.0;
        const double h = 0.5 * dt;
        const double mx = x + h * (sigma * (y - x));
        const double my = y + h * (x * (rho - z) - y);
        const double mz = z + h * (x * y - beta * z);
        x += dt * (sigma * (my - mx));
        y += dt * (mx * (rho - mz) - my);
        z += dt * (mx * my - beta * mz);
        const double ax = x < 0.0 ? -x : x;
        const double ay = y < 0.0 ? -y : y;
        const double az = z < 0.0 ? -z : z;
        // Also catches NaN (comparisons false → !(a <= 1e3) true).
        if (! (ax <= 1000.0 && ay <= 1000.0 && az <= 1000.0))
            reset();
    }
};

// ---------------------------------------------------------------------------
// Duffing oscillator (double-well, periodically forced):
//   x'' = -δx' + x - x³ + γ·cos(ω_f·t),  δ = 0.3, ω_f = 1.2 (Holmes' setup).
// Time is scaled so one forcing period lasts 1/hz seconds; the response
// therefore moves at (or chaotically below) `hz`. γ maps `chaos` 0–1 to
// 0.20 (period-1, single well) … 0.50 (cross-well chaos). The forcing cos
// comes from a renormalised quadrature rotator; a one-pole DC blocker at
// hz/10 removes the well offset so the output is audibly moving, not DC.
// Integration: semi-implicit Euler, ≤ kMaxSubsteps substeps of ≤ 0.1 units.

struct DuffingState
{
    static constexpr int kMaxSubsteps = 4;
    static constexpr double kMaxSubstepDt = 0.1;
    static constexpr double kForcingOmega = 1.2;
    static constexpr double kDamping = 0.3;

    double x = 0.1;  // position
    double v = 0.0;  // velocity
    double c = 1.0;  // forcing rotator cos
    double s = 0.0;  // forcing rotator sin
    double dcPrevIn = 0.0;
    double dcPrevOut = 0.0;

    void reset() noexcept
    {
        x = 0.1;
        v = 0.0;
        c = 1.0;
        s = 0.0;
        dcPrevIn = 0.0;
        dcPrevOut = 0.0;
    }

    // rotCos/rotSin: per-sample forcing rotation; h/steps: substep size and
    // count; dcR: DC-blocker pole. All precomputed by the wrapper when hz
    // changes. Returns the DC-blocked position.
    double step (double gamma, double rotCos, double rotSin, double h,
                 int steps, double dcR) noexcept
    {
        const double f = gamma * c;
        for (int i = 0; i < steps; ++i)
        {
            const double accel = -kDamping * v + x - x * x * x + f;
            v += accel * h;
            x += v * h;
        }

        const double nc = c * rotCos - s * rotSin;
        const double ns = c * rotSin + s * rotCos;
        const double g = 1.5 - 0.5 * (nc * nc + ns * ns); // keep |rotor| = 1
        c = nc * g;
        s = ns * g;

        const double ax = x < 0.0 ? -x : x;
        const double av = v < 0.0 ? -v : v;
        if (! (ax <= 100.0 && av <= 100.0))
        {
            reset();
            return 0.0;
        }

        const double y = x - dcPrevIn + dcR * dcPrevOut;
        dcPrevIn = x;
        dcPrevOut = y;
        return y;
    }
};

} // namespace vibesynth
