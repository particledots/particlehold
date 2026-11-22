// The slew. Plain C++, no plugin code. Our own maths, using Bryn's Curve (unchanged, its Log shape).
//
// Slew off: the output takes each new value at its sample (a step).
// Slew on (5 ms to 10 s): each new value is reached by a glide that starts fast and slows as it arrives, like the
// module's analog slew (a capacitor charging through the knob's resistance). A big jump and a small jump take the same
// time: the glide arrives exactly Slew after the sample moment (to within one sample), at any block size and sample
// rate. A new value during a glide starts a new glide from where the output is, with no jump. While hold and track
// follows a moving input, a glide restarts at every sample, so the output chases the input smoothly: the "inverse low
// pass filter" of the manual.

#pragma once

#include "particlehold/Curve.h"

#include <cmath>

namespace particlehold
{
    inline constexpr double kSlewMinSeconds = 0.005, kSlewMaxSeconds = 10.0;

    class Glide
    {
    public:
        struct Out { double level; bool corner; };   // corner: a step taken, or a glide arrived, at this sample

        void prepare (double sampleRate)
        {
            sr = sampleRate > 0.0 ? sampleRate : 48000.0;
            curve.setShape (1.0);
            setTime (seconds);
            jump (lvl);
        }

        // Seconds; 0 (or less than 1 sample) = off. A glide in progress carries on from where it is, with the new time.
        void setTime (double s)
        {
            seconds = s < 0.0 ? 0.0 : s;
            const long long newN = seconds <= 0.0 ? 0 : samplesFor (seconds);
            if (newN == length) return;
            length = newN;
            if (gliding)
            {
                if (length <= 1) { lvl = target; gliding = false; pendingCorner = true; }
                else startGlide();
            }
        }

        // Straight there (rest): no glide, no corner.
        void jump (double v) { lvl = target = v; gliding = false; pendingCorner = false; }

        // A new value at this sample (before next()). step: a sample moment (sample and hold, or hold and track
        // locking): with the slew off it is sent at its exact sample. A tracked value (step = false) is not.
        void setTarget (double v, bool step)
        {
            if (length <= 1)
            {
                target = lvl = v;
                gliding = false;
                if (step) pendingCorner = true;
                return;
            }
            if (same (v, target) && (gliding || same (v, lvl))) return;   // same value: the glide (if any) carries on
            target = v;
            startGlide();
        }

        Out next()
        {
            if (gliding)
            {
                ++n;
                if (n >= length)
                {
                    lvl = target;
                    gliding = false;
                    pendingCorner = false;
                    return { lvl, true };
                }
                lvl = start + (target - start) * curve ((double) n / (double) length);
            }
            const bool c = pendingCorner;
            pendingCorner = false;
            return { lvl, c };
        }

        double level() const   { return lvl; }
        double goal() const    { return target; }
        bool isGliding() const { return gliding; }
        long long lengthSamples() const { return length; }

        static long long samplesFor (double s, double sampleRate)
        {
            const long long k = (long long) std::llround (s * sampleRate);
            return k < 1 ? 1 : k;
        }

    private:
        static bool same (double a, double b) { return ! (a < b) && ! (a > b); }
        long long samplesFor (double s) const { return samplesFor (s, sr); }
        void startGlide() { start = lvl; n = 0; gliding = true; pendingCorner = false; }

        Curve curve;
        double sr = 48000.0, seconds = 0.0;
        long long length = 0, n = 0;
        double lvl = 0.5, target = 0.5, start = 0.5;
        bool gliding = false, pendingCorner = false;
    };
}
