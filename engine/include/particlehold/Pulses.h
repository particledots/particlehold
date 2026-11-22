// The Clock output. Plain C++, no plugin code. Adapted from Bryn 0.1.0's Pulses (same rules for length, merging,
// sending and stuck-note safety, taken from Hillside); only the source is new.
//
// Rules (shown in the README):
//   - Clock: high while the clock that drives the sample and hold is high (the internal clock's first half of each
//     tick, the held notes, or the Trigger button). A high shorter than 20 ms is stretched to 20 ms. A new rising edge
//     while it is still high (back-to-back notes) merges into one longer gate.
//   - Sent as (the "Send as" control): CC (127 while high, then 0), a note (velocity 100, released when low), or both,
//     on the MIDI out channel. Numbers in Outputs.h.
//   - Stuck-note safety: whatever is on is ended (CC 0 and/or note-off, on the channel it was sent on) when Send as or
//     the channel changes, when the output is switched off, and on rest (stop, reset, bypass, prepare). An output
//     that was ended that way is not started again until the clock next rises: changing a control never starts a note.

#pragma once

#include "particlehold/CcStream.h"
#include "particlehold/Outputs.h"

namespace particlehold
{
    class Pulses
    {
    public:
        // The pulse length. What is on stays on (and is ended by closeAll, which the Engine calls next).
        void prepare (double sampleRate);

        // Settings, applied at a block's first sample (offset). Ends what must end (see above).
        void configure (const bool enabled[kNumPulseOutputs], int channel, int sendAs, int offset, MidiSink& sink);

        // Every sample: whether the clock is high at this sample, and whether it rose here (a new edge).
        void step (long long absSample, int offset, bool high, bool rose, MidiSink& sink);

        // Ends everything that is on, at this offset.
        void closeAll (int offset, MidiSink& sink);

        bool isOn (int k) const       { return outs[k].cc || outs[k].note; }
        unsigned starts (int k) const { return outs[k].starts; }   // how many times it has been switched on
        bool anyOn() const;

    private:
        struct Out
        {
            bool cc = false, note = false;   // what is on now
            int channel = kDefaultChannel;   // where it was sent
            long long until = 0;             // stays on at least until this sample
            bool source = false;             // the clock is high now
            unsigned starts = 0;
        };
        void open (int k, long long absSample, int offset, MidiSink& sink);
        void close (int k, int offset, MidiSink& sink);

        Out outs[kNumPulseOutputs];
        bool enabled[kNumPulseOutputs] = { true };
        int channel = kDefaultChannel;
        int mode = sendAsCc;
        long long length = 960;   // 20 ms in samples
    };
}
