// From Bryn 0.1.0 (itself from Hillside 0.2.0). One addition for particlehold: each lane has a rest value (setRest),
// which is what switching a lane off and changing the channel send (Bryn's lanes rest at 0, the default, so with rest 0
// this is exactly Bryn's CcStream). particlehold's outputs rest at 0 V: the middle (8192), or the bottom (0) for the
// Signal output while the rectifier is on.
// Turns the envelope's values into a thin, exact stream of 14-bit MIDI CC (coarse + fine, 16,384 steps).
// Built on the Ogdoad and Strange Drift (0.1.1) CC stream, with one more rule for envelopes ("corners").
//
// Rules, per output ("lane"):
//   - at most ONE value per audio block;
//   - a value is sent once it has moved at least kMinStep (32 of 16,384 = 0.2%) and at least kMinIntervalSeconds
//     (6 ms) have passed since the last one; a smaller move is sent once kQuietSeconds (100 ms) have passed;
//     nothing is sent while the value stands still;
//   - corners: when a stage reaches its target (the top of the attack, the sustain level, the final 0) or the engine
//     goes to rest, that exact value is the one sent in that block, at that exact sample, whatever the 6 ms rule says.
//     So a 3 ms attack still reaches the very top, and every envelope ends on exactly 0;
//   - within a block the first sample the rule allows is used, at that exact sample;
//   - the coarse part is sent only when it changed, immediately before the fine part at the same sample; the fine part
//     is always sent, so a receiver acts when the fine part arrives.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace particlehold
{
    struct MidiSink
    {
        virtual ~MidiSink() = default;
        virtual void cc (int sampleOffset, int channel /*1..16*/, int number, int value) = 0;
        virtual void note (int /*sampleOffset*/, int /*channel*/, int /*number*/, int /*velocity, 0 = off*/) {}
    };

    inline constexpr int kMaxLanes = 8;
    inline constexpr double kMinIntervalSeconds = 0.006;
    inline constexpr double kQuietSeconds = 0.100;
    inline constexpr int kMinStep = 32;

    // 0..1 to 0..16383
    inline int toFourteenBit (double v)
    {
        double x = v * 16383.0 + 0.5;
        if (x < 0.0) x = 0.0;
        if (x > 16383.0) x = 16383.0;
        return (int) x;
    }

    class CcStream
    {
    public:
        void prepare (double sampleRate);
        void setLane (int lane, int coarseCc, int fineCc);
        void setRest (int lane, int value14) { lanes[(std::size_t) lane].rest = value14; }   // default 0
        int rest (int lane) const { return lanes[(std::size_t) lane].rest; }

        // Every sample: the lane's value now. absSample counts samples since prepare (never restarts within a run).
        void consider (int lane, long long absSample, int offset, int value14, bool corner);

        // Send this value as a corner (at rest: 0). Used for rest, switching a lane off.
        void force (int lane, long long absSample, int offset, int value14);

        // The lane is switched on or off. Switching off sends its rest value (if it was not already there) and then
        // nothing; switching on sends the current value at the next opportunity.
        void setEnabled (int lane, bool on, long long absSample, int offset);
        bool isEnabled (int lane) const { return lanes[(std::size_t) lane].enabled; }

        // Channel change: every lane not at its rest value sends it on the old channel at once; then everything is sent afresh on
        // the new channel.
        void moveChannel (int oldChannel, int offset, MidiSink& sink);

        // End of a block: send what was chosen, in sample order.
        void flush (int channel, MidiSink& sink);

        void forget();   // forget what was sent: everything is sent afresh

        int lastSent (int lane) const { return lanes[(std::size_t) lane].last; }
        int numLanes() const { return count; }

    private:
        struct Lane
        {
            int coarseCc = -1, fineCc = -1;
            int rest = 0;
            int last = -1, lastCoarse = -1;
            long long lastAbs = -(1LL << 40);
            bool enabled = true;
            bool has = false, hasCorner = false;
            int candOffset = 0, candValue = 0;
            long long candAbs = 0;
        };
        std::array<Lane, kMaxLanes> lanes {};
        int count = 0;
        long long minInterval = 288, quiet = 4800;   // in samples
    };
}
