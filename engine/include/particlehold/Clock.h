// The internal clock. Plain C++, no plugin code. Our own.
//
// A square wave: high for the first half of each tick, low for the second (the manual gives no duty cycle; hold and
// track needs one). Worked out sample by sample from a phase that counts samples since the last change of rate, so
// a tick lands on the same moment (to within one sample) at any block size and any sample rate: tick k is at
// (k + 0.5) / rate seconds after a restart.
// A restart (prepare, rest) starts at the low half: the first tick comes half a tick later, so the rest value is
// always sent first. Changing the rate keeps the phase: no jump, no extra tick.

#pragma once

#include <cmath>

namespace particlehold
{
    inline constexpr double kRateLowMin = 0.05, kRateLowMax = 4.0;     // Hz, Range Low
    inline constexpr double kRateHighMin = 0.5, kRateHighMax = 40.0;   // Hz, Range High

    // The Rate knob (0 .. 1) to Hz: equal ratios for equal turns (the centre is 0.45 Hz Low, 4.5 Hz High).
    inline double rateHz (double knob, bool high)
    {
        knob = knob < 0.0 ? 0.0 : (knob > 1.0 ? 1.0 : knob);
        const double lo = high ? kRateHighMin : kRateLowMin, hi = high ? kRateHighMax : kRateLowMax;
        return lo * std::pow (hi / lo, knob);
    }

    class Clock
    {
    public:
        void prepare (double sampleRate) { sr = sampleRate > 0.0 ? sampleRate : 48000.0; setRate (hz); restart(); }

        void setRate (double newHz)
        {
            if (newHz <= 0.0) newHz = 0.001;
            if (! (newHz < hz) && ! (newHz > hz) && n == 0) { inc = hz / sr; return; }   // the same rate
            base = phase();
            n = 0;
            hz = newHz;
            inc = hz / sr;
        }

        void restart() { base = 0.5; n = 0; }

        // High at this sample? Then one sample on.
        bool next()
        {
            const bool high = phase() < 0.5;
            ++n;
            if (n >= (1LL << 40)) { base = phase(); n = 0; }
            return high;
        }

        double phase() const
        {
            const double p = base + (double) n * inc;
            return p - std::floor (p);
        }
        double rate() const { return hz; }

    private:
        double sr = 48000.0, hz = 1.0, inc = 1.0 / 48000.0, base = 0.5;
        long long n = 0;
    };
}
