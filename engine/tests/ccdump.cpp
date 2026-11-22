// Prints the engine's real MIDI stream, for the mapper test: "sample channel cc value" per CC, and
// "note sample channel number velocity" per note (the Clock in Note or Both mode).
// Plays a fixed pattern of notes (short, long, overlapping) and a moving mod wheel through phases of sample and hold
// and hold and track, Notes and Internal clock, slew off and on, noise and the mod wheel as the source, rectifier off
// and on, with the Noise output on. At the end: a rest (as Logic's stop), so everything ends at 0 V.
// Usage: particlehold_ccdump [seconds] [out channel] [send as: 0 CC, 1 Note, 2 Both]

#include "particlehold/Engine.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace particlehold;

struct Printer : MidiSink
{
    long long blockStart = 0;
    void cc (int offset, int channel, int number, int v) override { std::printf ("%lld %d %d %d\n", blockStart + offset, channel, number, v); }
    void note (int offset, int channel, int number, int v) override { std::printf ("note %lld %d %d %d\n", blockStart + offset, channel, number, v); }
};

int main (int argc, char** argv)
{
    const double seconds = argc > 1 ? std::atof (argv[1]) : 30.0;
    const int channel = argc > 2 ? std::atoi (argv[2]) : kDefaultChannel;
    const int sendAs = argc > 3 ? std::atoi (argv[3]) : 0;
    const double sr = 48000.0;
    Engine e;
    e.setSeed (2026);
    e.prepare (sr);
    Settings s;
    s.outChannel = channel;
    s.sendAs = sendAs;
    s.outOn[outNoise] = true;
    e.setSettings (s);
    e.panic();
    Printer p;
    const long long total = (long long) (seconds * sr);
    std::vector<InputEvent> buf;
    long long pos = 0;
    int phase = 0;
    while (pos < total)
    {
        const int n = (int) (total - pos > 512 ? 512 : total - pos);
        buf.clear();
        for (int i = 0; i < n; ++i)
        {
            const long long t = pos + i, cycle = (long long) (1.0 * sr), x = t % cycle;
            if (x % 480 == 0) buf.push_back ({ i, InputEvent::controller, 0, 1, (int) ((t / 480) % 128) });   // the mod wheel moves
            if (x == 0) buf.push_back ({ i, InputEvent::noteOn, 0, (std::uint8_t) (48 + (t / cycle) % 24), 100 });
            if (x == (long long) (0.4 * sr)) buf.push_back ({ i, InputEvent::noteOff, 0, (std::uint8_t) (48 + (t / cycle) % 24), 0 });
            if (x == (long long) (0.6 * sr)) buf.push_back ({ i, InputEvent::noteOn, 1, 70, 90 });
            if (x == (long long) (0.62 * sr)) buf.push_back ({ i, InputEvent::noteOff, 1, 70, 0 });
        }
        const int newPhase = (int) (pos / (long long) (5.0 * sr));
        if (newPhase != phase)
        {
            phase = newPhase;
            s.mode = phase % 2 == 0 ? modeSampleHold : modeHoldTrack;
            s.clock = phase % 3 == 2 ? clockInternal : clockNotes;
            s.rateHz = phase % 4 < 2 ? 3.3 : 17.0;
            s.slew = phase % 4 == 1 ? 0.08 : 0.0;
            s.source = phase % 5 == 3 ? sourceCc : sourceNoise;
            s.rectify = phase % 6 == 4;
            s.level = phase % 7 == 5 ? 0.6 : 1.0;
            e.setSettings (s);
        }
        p.blockStart = pos;
        e.render (n, buf.data(), (int) buf.size(), false, p);
        pos += n;
    }
    // Notes as the clock and nothing played for a second (every Clock gate ends by itself), then a rest (as at a
    // transport stop); report where the outputs ended (14-bit), for the test.
    s.rectify = false;
    s.clock = clockNotes;
    e.setSettings (s);
    for (int k = 0; k < 94; ++k) { p.blockStart = pos; e.render (512, nullptr, 0, false, p); pos += 512; }
    e.panic();
    p.blockStart = pos;
    e.render (512, nullptr, 0, false, p);   // one block: the rest is sent (the Noise output would run on after it)
    std::printf ("final 0 %d\n", e.ccStream().lastSent (outSignal));
    std::printf ("final 1 %d\n", e.ccStream().lastSent (outNoise));
    std::printf ("pulses-on %d\n", e.pulses().anyOn() ? 1 : 0);
    return 0;
}
