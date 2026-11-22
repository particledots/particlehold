// The particlehold engine self-test. Runs without Logic; the build workflow runs it on every push and fails if any
// check fails. Built the same way as Bryn's engine test.
//
//   1. Defaults: dropped in and played, each note takes a new value; Signal, Clock as CC on channel 13; Noise silent.
//   2. Timing: every sample moment, clock tick and slew lands on the same sample at every block size (32 to 1024 and
//      random), and on the same moment (to within one sample) at 44.1, 48 and 96 kHz.
//   3. Sample and hold, hold and track, Level, the rectifier, the 0 V point, every source (noise, CC with 14-bit
//      pairs, aftertouch, pitch bend, velocity), the slew (shape, same time for any jump, no jump on a new value).
//   4. The clock: Rate and Range, Internal / Notes / Button only, the Trigger button with any clock, rate changes.
//   5. Notes (Bryn's Keys): chords, legato, back-to-back, zero-length notes, All Notes Off, a missing note-off.
//   6. Rest (stop, reset, bypass): 0 V and the Clock gate ended at once, keys forgotten, the next note plays.
//   7. Control changes never make a sample moment or start the Clock gate; outputs switched off send 0 V once.
//   8. MIDI: at most one value per output per block, 6 ms / 0.2% rule, every step reaching Logic up to 40 Hz at a
//      1024 buffer, traffic measured; the noise evenly spread; CPU measured.
//   9. Fuzz: random notes, controllers, blocks (0 and 1 samples too), settings, rests: nothing stuck.

#include "particlehold/Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <vector>

using namespace particlehold;

static int checks = 0, failures = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        ++checks;                                                          \
        if (! (cond)) {                                                    \
            std::printf ("FAIL line %d: %s   ", __LINE__, #cond);          \
            std::printf (__VA_ARGS__);                                     \
            std::printf ("\n");                                            \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

// ------------------------------------------------------------------------------------------------ harness

struct Msg { long long at; int ch, num, val; bool note; };

struct Rec final : MidiSink
{
    long long base = 0;
    std::vector<Msg> m;
    void cc (int o, int ch, int n, int v) override   { m.push_back ({ base + o, ch, n, v, false }); }
    void note (int o, int ch, int n, int v) override { m.push_back ({ base + o, ch, n, v, true }); }
};

struct Ev { long long at; InputEvent::Kind kind; int channel, number, value; };

struct Script
{
    long long length = 48000;
    std::vector<Ev> events;
    std::vector<std::pair<long long, Settings>> changes;   // applied at the start of the block containing them
    std::vector<std::pair<long long, bool>> manual;        // the Trigger button from that sample's block on
    std::vector<long long> rests;                          // panic() before the block containing them
    std::vector<long long> bypassFrom, bypassTo;           // bypassed blocks (flushOnly)

    void note (long long at, long long len, int nt = 60, int vel = 100, int ch = 0)
    {
        events.push_back ({ at, InputEvent::noteOn, ch, nt, vel });
        events.push_back ({ at + len, InputEvent::noteOff, ch, nt, 0 });
    }
    void ccAt (long long when, int n, int v, int ch = 0) { events.push_back ({ when, InputEvent::controller, ch, n, v }); }
    void bendAt (long long when, int v) { events.push_back ({ when, InputEvent::pitchBend, 0, 0, v }); }
    void sortEvents() { std::stable_sort (events.begin(), events.end(), [] (const Ev& a, const Ev& b) { return a.at < b.at; }); }
};

struct Result
{
    std::vector<Msg> msgs;
    std::vector<TracePoint> trace;
    std::vector<long long> blockStarts;
    unsigned edges = 0;
};

static Result play (Script sc, double sr, std::function<int (long long)> blockSize, Settings init, bool withTrace = true,
                    std::uint64_t seed = 12345, Engine* keep = nullptr)
{
    sc.sortEvents();
    Engine local;
    Engine& e = keep != nullptr ? *keep : local;
    e.setSeed (seed);
    e.prepare (sr);
    e.setSettings (init);
    e.panic();
    Result r;
    if (withTrace) { r.trace.reserve ((size_t) sc.length); e.trace = &r.trace; }
    Rec rec;
    std::vector<InputEvent> buf;
    size_t ev = 0, ch = 0, rs = 0;
    bool manual = false;
    long long pos = 0;
    while (pos < sc.length)
    {
        int n = blockSize (pos);
        if (n > sc.length - pos) n = (int) (sc.length - pos);
        while (ch < sc.changes.size() && sc.changes[ch].first < pos + (n > 0 ? n : 1)) { e.setSettings (sc.changes[ch].second); ++ch; }
        while (rs < sc.rests.size() && sc.rests[rs] < pos + (n > 0 ? n : 1)) { e.panic(); ++rs; }
        for (auto& m : sc.manual) if (m.first <= pos) manual = m.second;
        bool bypassed = false;
        for (size_t k = 0; k < sc.bypassFrom.size(); ++k) if (pos >= sc.bypassFrom[k] && pos < sc.bypassTo[k]) bypassed = true;
        buf.clear();
        while (ev < sc.events.size() && sc.events[ev].at < pos + n)
        {
            const auto& x = sc.events[ev++];
            if (bypassed) continue;
            InputEvent ie { (int) (x.at - pos), x.kind, (std::uint8_t) x.channel, (std::uint8_t) x.number, x.value };
            buf.push_back (ie);
        }
        rec.base = pos;
        r.blockStarts.push_back (pos);
        if (bypassed) e.flushOnly (n, rec);
        else          e.render (n, buf.data(), (int) buf.size(), manual, rec);
        pos += n;
    }
    e.trace = nullptr;
    r.msgs = std::move (rec.m);
    r.edges = e.edges();
    return r;
}

static std::function<int (long long)> fixedBlock (int n) { return [n] (long long) { return n; }; }
static std::function<int (long long)> randomBlocks (unsigned seed)
{
    auto rng = std::make_shared<std::mt19937> (seed);
    return [rng] (long long) { return 1 + (int) ((*rng)() % 1024); };
}
static long long at (double seconds, double sr) { return (long long) std::llround (seconds * sr); }

// What a receiver (the mapper) knows: the latest 14-bit value of a pair on a channel, and pulse CC / notes.
struct Receiver
{
    int coarse[17][128] {};
    int value[17][128];
    int ccVal[17][128] {};
    bool noteOn[17][128] {};
    int doubled = 0, strayOff = 0;
    Receiver() { for (auto& c : value) for (auto& v : c) v = -1; }
    void take (const Msg& m)
    {
        if (m.note)
        {
            if (m.val > 0) { doubled += noteOn[m.ch][m.num] ? 1 : 0; noteOn[m.ch][m.num] = true; }
            else           { strayOff += noteOn[m.ch][m.num] ? 0 : 1; noteOn[m.ch][m.num] = false; }
            return;
        }
        ccVal[m.ch][m.num] = m.val;
        for (int k = 0; k < kNumSmoothOutputs; ++k)
        {
            if (m.num == kCoarseCc[k]) coarse[m.ch][k] = m.val;
            if (m.num == kFineCc[k])   value[m.ch][k] = coarse[m.ch][k] * 128 + m.val;
        }
    }
    void take (const std::vector<Msg>& v) { for (auto& m : v) take (m); }
    bool clockOn() const
    {
        for (int ch = 1; ch <= 16; ++ch) if (ccVal[ch][kPulseCc[0]] != 0 || noteOn[ch][kPulseNote[0]]) return true;
        return false;
    }
};

// The Signal value as the receiver meant it, at each fine message: (sample, value).
static std::vector<std::pair<long long, int>> values (const std::vector<Msg>& msgs, int lane, int ch = kDefaultChannel)
{
    std::vector<std::pair<long long, int>> v;
    int c = 0;
    for (auto& m : msgs)
    {
        if (m.note || m.ch != ch) continue;
        if (m.num == kCoarseCc[lane]) c = m.val;
        if (m.num == kFineCc[lane]) v.push_back ({ m.at, c * 128 + m.val });
    }
    return v;
}
static int countCc (const std::vector<Msg>& msgs, int num, int val = -1)
{
    int n = 0;
    for (auto& m : msgs) if (! m.note && m.num == num && (val < 0 || m.val == val)) ++n;
    return n;
}
static std::vector<long long> edgesOf (const std::vector<TracePoint>& t)
{
    std::vector<long long> v;
    for (size_t i = 0; i < t.size(); ++i) if (t[i].edge) v.push_back ((long long) i);
    return v;
}
static double expectedFor (int v14, double level, bool rect)   // the output for a 14-bit source value
{
    const double y = ((double) v14 * (2.0 / 16383.0) - 1.0) * level;
    return rect ? std::fabs (y) : (y + 1.0) * 0.5;
}
// At most one value per output per block, in every result.
static int overBlock (const Result& r)
{
    int bad = 0;
    std::map<std::pair<long long, int>, int> count;
    size_t b = 0;
    for (auto& m : r.msgs)
    {
        while (b + 1 < r.blockStarts.size() && r.blockStarts[b + 1] <= m.at) ++b;
        if (! m.note && (m.num == kFineCc[0] || m.num == kFineCc[1])) ++count[{ r.blockStarts[b] * 4 + m.ch, m.num }];
    }
    for (auto& [k, c] : count) bad += c > 1 ? 1 : 0;
    return bad;
}

// ------------------------------------------------------------------------------------------------ 1. defaults

static void testDefaults()
{
    const double sr = 48000.0;
    Settings s;   // the defaults
    CHECK (s.mode == modeSampleHold && s.level == 1.0 && ! s.rectify && s.slew == 0.0 && s.clock == clockNotes
           && s.source == sourceNoise && s.outChannel == 13 && s.outOn[outSignal] && ! s.outOn[outNoise] && s.pulseOn[0]
           && s.sendAs == sendAsCc, "defaults: sample and hold, Level 100%%, rectifier off, slew off, Notes, Noise, ch 13, Signal + Clock on, Noise off, CC");
    Script sc;
    sc.length = at (2.0, sr);
    for (int k = 0; k < 6; ++k) sc.note (at (0.1 + 0.3 * k, sr), at (0.1, sr), 60 + k);
    auto r = play (sc, sr, fixedBlock (256), s);
    auto sig = values (r.msgs, outSignal);
    CHECK (! sig.empty() && sig[0].first == 0 && sig[0].second == kMiddle14, "rest (0 V, the middle 8192) sent at the start");
    int atNotes = 0;
    for (int k = 0; k < 6; ++k)
        for (auto& v : sig) if (v.first == at (0.1 + 0.3 * k, sr)) ++atNotes;
    CHECK (atNotes == 6 && sig.size() == 7, "each of 6 notes sends a new value at its exact sample (%d at notes, %d values)", atNotes, (int) sig.size());
    std::vector<int> distinct;
    for (auto& v : sig) distinct.push_back (v.second);
    std::sort (distinct.begin(), distinct.end());
    CHECK (std::unique (distinct.begin(), distinct.end()) - distinct.begin() == 7, "the values differ (random)");
    CHECK (countCc (r.msgs, 79, 127) == 6 && countCc (r.msgs, 79, 0) == 6, "Clock: CC 79 127 at each note, 0 after it");
    bool onlyCh13 = true, noNoise = true, noNotes = true;
    for (auto& m : r.msgs) { onlyCh13 = onlyCh13 && m.ch == 13; noNoise = noNoise && m.num != 77 && m.num != 78; noNotes = noNotes && ! m.note; }
    CHECK (onlyCh13 && noNoise && noNotes, "everything on channel 13; Noise (77 / 78) silent; no notes (Send as CC)");
    // Values held between notes: the trace only changes at the notes.
    int changes = 0;
    for (size_t i = 1; i < r.trace.size(); ++i) changes += r.trace[i].level != r.trace[i - 1].level ? 1 : 0;
    CHECK (changes == 6, "the value is held between notes (%d changes)", changes);
}

// ------------------------------------------------------------------------------------------------ 2. timing

static Script timingScript (double sr, bool withNotes)
{
    Script sc;
    sc.length = at (6.0, sr);
    if (withNotes)
    {
        const double t[] = { 0.11, 0.37, 0.52, 0.523, 1.25, 2.001, 2.9, 3.33, 4.5, 5.2 };
        const double len[] = { 0.05, 0.1, 0.01, 0.3, 0.2, 0.4, 0.01, 0.6, 0.25, 0.3 };
        for (int k = 0; k < 10; ++k) sc.note (at (t[k], sr), at (len[k], sr), 50 + k);
    }
    for (int k = 0; k < 60; ++k) sc.bendAt (at (0.05 + k * 0.1, sr), (k * 2741) % 16384);
    return sc;
}

static void testTiming()
{
    int worst = 0, runs = 0;
    // a) Block size: the same trace, sample for sample, at every block size (each setting group, at 48 kHz).
    struct Group { const char* name; Settings s; bool notes; };
    std::vector<Group> groups;
    { Settings s; s.clock = clockInternal; s.rateHz = 7.3; s.slew = 0.05; s.source = sourcePitchBend; groups.push_back ({ "internal 7.3 Hz, slew 50 ms, pitch bend", s, false }); }
    { Settings s; s.clock = clockInternal; s.rateHz = 23.0; s.source = sourceNoise; s.outOn[outNoise] = true; groups.push_back ({ "internal 23 Hz, noise", s, false }); }
    { Settings s; s.clock = clockNotes; s.slew = 0.2; s.source = sourceNoise; groups.push_back ({ "notes, slew 200 ms, noise", s, true }); }
    { Settings s; s.clock = clockNotes; s.mode = modeHoldTrack; s.slew = 0.03; s.source = sourcePitchBend; s.level = 0.7; s.rectify = true; groups.push_back ({ "notes, hold and track, slew 30 ms, rectified", s, true }); }
    { Settings s; s.clock = clockInternal; s.rateHz = 2.0; s.mode = modeHoldTrack; s.source = sourceNoise; groups.push_back ({ "internal 2 Hz, hold and track, noise", s, false }); }
    for (auto& g : groups)
    {
        const double sr = 48000.0;
        const auto sc = timingScript (sr, g.notes);
        const auto ref = play (sc, sr, fixedBlock (1), g.s);
        const std::vector<std::function<int (long long)>> sizes { fixedBlock (32), fixedBlock (64), fixedBlock (100), fixedBlock (128), fixedBlock (256),
                                                                  fixedBlock (441), fixedBlock (512), fixedBlock (1024), randomBlocks (9) };
        int same = 0;
        for (auto& bs : sizes)
        {
            const auto r = play (sc, sr, bs, g.s);
            bool eq = r.trace.size() == ref.trace.size();
            for (size_t i = 0; eq && i < r.trace.size(); ++i)
                eq = r.trace[i].level == ref.trace[i].level && r.trace[i].edge == ref.trace[i].edge && r.trace[i].gate == ref.trace[i].gate
                  && r.trace[i].corner == ref.trace[i].corner && r.trace[i].tracking == ref.trace[i].tracking;
            same += eq ? 1 : 0;
            ++runs;
            // Every step and every glide's arrival reaches the receiver at its exact sample.
            int missing = 0, corners = 0;
            const auto sig = values (r.msgs, outSignal);
            for (size_t i = 0; i < r.trace.size(); ++i)
            {
                if (! r.trace[i].corner || r.trace[i].tracking) continue;
                ++corners;
                const int want = toFourteenBit (r.trace[i].level);
                bool found = false;
                for (auto& v : sig) if (v.first == (long long) i && v.second == want) found = true;
                // A corner whose value was already the last one sent needs no message.
                if (! found)
                {
                    int lastBefore = -1;
                    for (auto& v : sig) if (v.first <= (long long) i) lastBefore = v.second;
                    found = lastBefore == want;
                }
                missing += found ? 0 : 1;
            }
            CHECK (missing == 0, "%s: every step and glide arrival sent at its exact sample (%d of %d missing)", g.name, missing, corners);
            CHECK (overBlock (r) == 0, "%s: at most one value per output per block", g.name);
        }
        CHECK (same == (int) sizes.size(), "%s: identical trace at every block size (%d of %d)", g.name, same, (int) sizes.size());
    }

    // b) Sample rate: ticks, note samples and glide arrivals at the same moment (within one sample) at 44.1/48/96 kHz.
    for (double hz : { 0.05 * 3, 0.45, 4.5, 13.7, 40.0 })
    {
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            Settings s; s.clock = clockInternal; s.rateHz = hz; s.slew = 0.0;
            Script sc; sc.length = at (hz < 1.0 ? 20.0 : 3.0, sr);
            const auto r = play (sc, sr, fixedBlock (256), s);
            const auto e = edgesOf (r.trace);
            int off = 0;
            for (size_t k = 0; k < e.size(); ++k)
            {
                const double want = ((double) k + 0.5) / hz * sr;   // sample index of tick k
                const double d = std::fabs ((double) e[k] - want);
                off += d > 1.0 ? 1 : 0;
                worst = std::max (worst, (int) std::ceil (d - 1e-9));
            }
            const int expectTicks = (int) std::floor ((double) sc.length / sr * hz - 0.5) + 1;
            CHECK (off == 0 && std::abs ((int) e.size() - expectTicks) <= 1, "internal %.2f Hz at %.0f Hz: %d ticks, each within one sample of (k + 0.5) / rate (%d off)", hz, sr, (int) e.size(), off);
            ++runs;
        }
    }
    for (double slew : { 0.005, 0.05, 0.4, 3.0 })
    {
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            Settings s; s.slew = slew; s.source = sourcePitchBend;
            Script sc; sc.length = at (slew * 3 + 1.0, sr);
            sc.bendAt (0, 16000);
            sc.note (at (0.25, sr), at (0.01, sr));
            const auto r = play (sc, sr, fixedBlock (512), s);
            long long edge = -1, arrive = -1;
            for (size_t i = 0; i < r.trace.size(); ++i)
            {
                if (r.trace[i].edge && edge < 0) edge = (long long) i;
                if (edge >= 0 && r.trace[i].corner && arrive < 0) arrive = (long long) i;
            }
            const double took = (double) (arrive - edge + 1) / sr;   // the samples the glide moves through
            CHECK (edge == at (0.25, sr) && std::fabs (took - slew) <= 1.0 / sr + 1e-9, "slew %.3f s at %.0f Hz: starts at the note, arrives %.6f s later (within one sample)", slew, sr, took);
            CHECK (std::fabs (r.trace[(size_t) arrive].level - expectedFor (16000, 1.0, false)) < 1e-12, "slew %.3f s at %.0f Hz: arrives exactly on the value", slew, sr);
            worst = std::max (worst, (int) std::ceil (std::fabs (took - slew) * sr - 1e-9));
            ++runs;
        }
    }
    // c) The same notes at every sample rate: each sample moment at the note's own sample.
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        Settings s;
        const auto sc = timingScript (sr, true);
        const auto r = play (sc, sr, fixedBlock (333), s);
        auto e = edgesOf (r.trace);
        std::vector<long long> want;
        for (auto& x : sc.events) if (x.kind == InputEvent::noteOn) want.push_back (x.at);
        std::sort (want.begin(), want.end());
        // 0.52 + 0.01 overlaps 0.523 + 0.3 (legato: no new edge).
        want.erase (std::remove (want.begin(), want.end(), at (0.523, sr)), want.end());
        CHECK (e == want, "notes at %.0f Hz: a sample moment at each note's exact sample, none for the legato note (%d / %d)", sr, (int) e.size(), (int) want.size());
        ++runs;
    }
    std::printf ("timing: %d runs (block sizes 1 to 1024 and random; 44.1, 48, 96 kHz): identical at every block size, worst %d sample off across rates\n", runs, worst);
}

// ------------------------------------------------------------------------------------------------ 3. signal path

static void testSignal()
{
    const double sr = 48000.0;
    // Identity: a sampled controller comes out at the value it went in (Level 100%, rectifier off).
    {
        Settings s; s.source = sourcePitchBend;
        Script sc; sc.length = at (1.0, sr);
        const int vals[] = { 0, 1, 4000, 8192, 12345, 16383 };
        for (int k = 0; k < 6; ++k) { sc.bendAt (at (0.1 * k + 0.05, sr), vals[k]); sc.note (at (0.1 * k + 0.06, sr), 100); }
        const auto sig = values (play (sc, sr, fixedBlock (128), s).msgs, outSignal);
        int ok = 0;
        for (int k = 0; k < 6; ++k) for (auto& v : sig) if (v.first == at (0.1 * k + 0.06, sr) && v.second == vals[k]) ++ok;
        CHECK (ok == 6, "pitch bend sampled: out exactly as in, 14-bit (%d of 6)", ok);
    }
    // Level and the rectifier: the 0 V point is the middle (off) or the bottom (on).
    for (bool rect : { false, true })
        for (double level : { 1.0, 0.5, 0.0 })
        {
            Settings s; s.source = sourcePitchBend; s.level = level; s.rectify = rect;
            Script sc; sc.length = at (0.5, sr);
            sc.bendAt (at (0.01, sr), 2000); sc.note (at (0.02, sr), 50);
            sc.bendAt (at (0.1, sr), 15000); sc.note (at (0.11, sr), 50);
            const auto r = play (sc, sr, fixedBlock (64), s);
            const double a = r.trace[(size_t) at (0.05, sr)].level, b = r.trace[(size_t) at (0.2, sr)].level;
            CHECK (std::fabs (a - expectedFor (2000, level, rect)) < 1e-12 && std::fabs (b - expectedFor (15000, level, rect)) < 1e-12,
                   "Level %.0f%%, rectifier %s: %.4f and %.4f as worked out", level * 100.0, rect ? "on" : "off", a, b);
            const auto sig = values (r.msgs, outSignal);
            CHECK (sig[0].second == (rect ? 0 : kMiddle14), "rectifier %s: rest at %s", rect ? "on" : "off", rect ? "the bottom (0)" : "the middle (8192)");
            if (rect && level == 1.0) CHECK (a > 0.7 && b > 0.8, "rectified: a low value folds up (%.3f), a high one stays (%.3f)", a, b);
            if (level == 0.0) CHECK (std::fabs (a - (rect ? 0.0 : 0.5)) < 1e-12, "Level 0: nothing passes (0 V)");
        }
    // Hold and track: follows the input while held, holds the value it had when the note ended.
    {
        Settings s; s.mode = modeHoldTrack; s.source = sourceCc; s.sourceCc = 20;
        Script sc; sc.length = at (1.5, sr);
        for (int k = 0; k < 120; ++k) sc.ccAt (at (0.01 * k, sr), 20, k);
        sc.note (at (0.2, sr), at (0.3, sr));
        const auto r = play (sc, sr, fixedBlock (256), s);
        const double during1 = r.trace[(size_t) at (0.255, sr)].level, during2 = r.trace[(size_t) at (0.405, sr)].level;
        const double after1 = r.trace[(size_t) at (0.6, sr)].level, after2 = r.trace[(size_t) at (1.4, sr)].level;
        CHECK (std::fabs (during1 - 25.0 * 129 / 16383.0) < 1e-9 && std::fabs (during2 - 40.0 * 129 / 16383.0) < 1e-9, "hold and track: follows the controller while held");
        CHECK (std::fabs (after1 - 49.0 * 129 / 16383.0) < 1e-9 && after2 == after1, "hold and track: holds the value from the last held sample (CC 49) after the note");
        const auto sig = values (r.msgs, outSignal);
        int lastSent = -1;
        for (auto& v : sig) if (v.first <= at (0.5, sr)) lastSent = v.second;
        bool laterSent = false;
        for (auto& v : sig) laterSent = laterSent || v.first > at (0.5, sr);
        CHECK (lastSent == 49 * 129 && ! laterSent, "hold and track: the held value has reached the receiver exactly by the note's end, nothing after");
        bool tracked = false;
        for (size_t i = 0; i < r.trace.size(); ++i) tracked = tracked || r.trace[i].tracking;
        CHECK (tracked && ! r.trace.back().tracking, "the track light follows (tracking while held, not after)");
        // The same in sample and hold: only the note's sample counts.
        s.mode = modeSampleHold;
        const auto r2 = play (sc, sr, fixedBlock (256), s);
        CHECK (std::fabs (r2.trace[(size_t) at (0.45, sr)].level - 20.0 * 129 / 16383.0) < 1e-9, "sample and hold: holds the value at the note (CC 20) all through it");
    }
    // Sources: CC as 7-bit, 14-bit pairs (standard and particledots), aftertouch, velocity, noise.
    {
        struct C { int src, cc; std::vector<std::pair<int, int>> sends; int want; const char* name; };
        const std::vector<C> cases {
            { sourceCc, 1,   { { 1, 100 } }, 100 * 129, "CC 1 (7-bit): 100 -> 100 x 129" },
            { sourceCc, 20,  { { 20, 40 }, { 52, 99 } }, 40 * 128 + 99, "CC 20 + 52 (Ogdoad LFO 1): full 14-bit" },
            { sourceCc, 80,  { { 80, 3 }, { 81, 127 } }, 3 * 128 + 127, "CC 80 + 81 (Bryn): full 14-bit" },
            { sourceCc, 106, { { 106, 127 }, { 113, 127 } }, 16383, "CC 106 + 113 (Strange Drift): full 14-bit" },
            { sourceCc, 3,   { { 3, 64 }, { 35, 1 } }, 8193, "CC 3 + 35 (Hillside): full 14-bit" },
            { sourceCc, 7,   { { 7, 127 } }, 16383, "CC 7: 127 is the very top" },
        };
        for (auto& c : cases)
        {
            Settings s; s.source = c.src; s.sourceCc = c.cc;
            Script sc; sc.length = at (0.2, sr);
            for (auto& x : c.sends) sc.ccAt (at (0.05, sr), x.first, x.second, 4);
            sc.note (at (0.05, sr), at (0.05, sr));
            const auto sig = values (play (sc, sr, fixedBlock (128), s).msgs, outSignal);
            CHECK (sig.size() >= 2 && sig[1].second == c.want, "source %s (got %d)", c.name, sig.size() >= 2 ? sig[1].second : -1);
        }
        Settings s; s.source = sourceAftertouch;
        Script sc; sc.length = at (0.3, sr);
        sc.events.push_back ({ at (0.05, sr), InputEvent::pressure, 2, 0, 90 });
        sc.note (at (0.06, sr), at (0.02, sr));
        auto sig = values (play (sc, sr, fixedBlock (128), s).msgs, outSignal);
        CHECK (sig.size() >= 2 && sig[1].second == 90 * 129, "source aftertouch: 90 -> 90 x 129");
        s.source = sourceVelocity;
        Script sv; sv.length = at (0.5, sr);
        sv.note (at (0.05, sr), at (0.02, sr), 60, 33);
        sv.note (at (0.2, sr), at (0.02, sr), 61, 127);
        sig = values (play (sv, sr, fixedBlock (128), s).msgs, outSignal);
        CHECK (sig.size() >= 3 && sig[1].second == 33 * 129 && sig[2].second == 16383, "source velocity: each note samples its own velocity (33, 127)");
    }
    // The slew: Log shape (fast, then slowing), same time for a big and a small jump, no jump on a new value mid-glide.
    {
        Settings s; s.source = sourcePitchBend; s.slew = 0.2;
        Script sc; sc.length = at (2.0, sr);
        sc.bendAt (0, 16383); sc.note (at (0.1, sr), 100);            // big jump: middle -> top
        sc.bendAt (at (0.5, sr), 14000); sc.note (at (0.5, sr), 100);  // small jump
        sc.bendAt (at (0.9, sr), 0); sc.note (at (0.9, sr), 100);      // big jump down ...
        sc.bendAt (at (0.95, sr), 16383); sc.note (at (0.95, sr), 100); // ... turned round mid-glide
        const auto r = play (sc, sr, fixedBlock (256), s);
        auto arrival = [&] (double from) { for (size_t i = (size_t) at (from, sr); i < r.trace.size(); ++i) if (r.trace[i].corner) return (long long) i - at (from, sr); return (long long) -1; };
        const long long a1 = arrival (0.1), a2 = arrival (0.5);
        CHECK (a1 == a2 && a1 == at (0.2, sr) - 1, "a big and a small jump take the same time (%lld, %lld samples)", a1, a2);
        const double start = r.trace[(size_t) at (0.1, sr) - 1].level, end = r.trace[(size_t) (at (0.1, sr) + a1)].level;
        const double half = r.trace[(size_t) at (0.2, sr)].level;
        CHECK ((half - start) / (end - start) > 0.85, "Log glide: most of the way after half the time (%.2f)", (half - start) / (end - start));
        double biggest = 0.0;
        for (size_t i = (size_t) at (0.05, sr); i < r.trace.size(); ++i) biggest = std::max (biggest, std::fabs (r.trace[i].level - r.trace[i - 1].level));
        CHECK (biggest < 0.002, "no jump anywhere while gliding, also when turned round mid-glide (largest step %.5f)", biggest);
        std::printf ("no jump: largest step between samples with slew 200 ms %.5f of full scale\n", biggest);
        // Slew changed mid-glide: carries on from where it is.
        Script sc2; sc2.length = at (1.0, sr);
        sc2.bendAt (0, 16383); sc2.note (at (0.1, sr), 100);
        Settings s2 = s; s2.slew = 1.0;
        sc2.changes.push_back ({ 7168, s2 });   // at a block boundary
        const auto r2 = play (sc2, sr, fixedBlock (256), s);
        double big2 = 0.0;
        for (size_t i = 1; i < r2.trace.size(); ++i) big2 = std::max (big2, std::fabs (r2.trace[i].level - r2.trace[i - 1].level));
        CHECK (big2 < 0.002 && r2.trace.back().level < expectedFor (16383, 1, false), "slew changed mid-glide: no jump, carries on more slowly");
        // Slew off mid-glide: straight to the value, sent exactly.
        Settings s3 = s; s3.slew = 0.0;
        Script sc3 = sc2; sc3.changes.clear(); sc3.changes.push_back ({ 7168, s3 });
        const auto r3 = play (sc3, sr, fixedBlock (256), s);
        CHECK (r3.trace[7168].level == expectedFor (16383, 1, false) && r3.trace[7168].corner, "slew switched off mid-glide: the value at once, sent exactly");
    }
    // Hold and track with the slew: chases the input smoothly (a low-pass), then settles on the held value.
    {
        Settings s; s.mode = modeHoldTrack; s.slew = 0.05; s.source = sourceNoise;
        Script sc; sc.length = at (1.0, sr);
        sc.note (at (0.1, sr), at (0.5, sr));
        const auto r = play (sc, sr, fixedBlock (256), s);
        double inVar = 0, outVar = 0, big = 0;
        for (size_t i = (size_t) at (0.3, sr); i < (size_t) at (0.6, sr); ++i)
        {
            inVar += std::fabs (r.trace[i].input - r.trace[i - 1].input);
            outVar += std::fabs (r.trace[i].level - r.trace[i - 1].level);
            big = std::max (big, std::fabs (r.trace[i].level - r.trace[i - 1].level));
        }
        CHECK (outVar < inVar / 50.0 && big < 0.01, "hold and track + slew on noise: smoothed (movement %.1f vs %.1f)", outVar, inVar);
        CHECK (r.trace[(size_t) at (0.9, sr)].level == r.trace.back().level, "and holds after the note");
    }
}

// ------------------------------------------------------------------------------------------------ 4. clock

static void testClock()
{
    const double sr = 48000.0;
    // Rate and Range.
    CHECK (std::fabs (rateHz (0.0, false) - 0.05) < 1e-12 && std::fabs (rateHz (1.0, false) - 4.0) < 1e-12 && std::fabs (rateHz (0.5, false) - 0.4472) < 1e-3,
           "Range Low: 0.05 to 4 Hz, centre 0.45 Hz");
    CHECK (std::fabs (rateHz (0.0, true) - 0.5) < 1e-12 && std::fabs (rateHz (1.0, true) - 40.0) < 1e-12 && std::fabs (rateHz (0.5, true) - 4.472) < 1e-3,
           "Range High: 0.5 to 40 Hz, centre 4.5 Hz");
    // Internal ignores notes; Notes ignores the internal clock; Button only ignores both.
    Script sc; sc.length = at (2.0, sr);
    sc.note (at (0.3, sr), at (0.2, sr)); sc.note (at (1.3, sr), at (0.2, sr));
    sc.manual = { { at (1.7, sr) / 256 * 256, true }, { at (1.8, sr) / 256 * 256, false } };
    {
        Settings s; s.clock = clockInternal; s.rateHz = 4.0;
        const auto r = play (sc, sr, fixedBlock (256), s);
        auto e = edgesOf (r.trace);
        int notesAt = 0;
        for (auto x : e) notesAt += (x == at (0.3, sr) || x == at (1.3, sr)) ? 1 : 0;
        CHECK (notesAt == 0 && (int) e.size() >= 8 && (int) e.size() <= 9, "Internal: 4 Hz ticks, notes ignored (%d edges)", (int) e.size());
    }
    {
        Settings s; s.clock = clockNotes;
        const auto r = play (sc, sr, fixedBlock (256), s);
        CHECK (edgesOf (r.trace).size() == 3, "Notes: one edge per note, plus the Trigger button (%d)", (int) edgesOf (r.trace).size());
    }
    {
        Settings s; s.clock = clockButton;
        const auto r = play (sc, sr, fixedBlock (256), s);
        auto e = edgesOf (r.trace);
        CHECK (e.size() == 1 && e[0] == at (1.7, sr) / 256 * 256, "Button only: notes and the internal clock ignored, the Trigger button samples");
    }
    // The Trigger button while the internal clock is low makes an edge; while it is high adds nothing (one gate).
    {
        Settings s; s.clock = clockInternal; s.rateHz = 1.0;   // high from 0.5 s to 1.0 s, low 1.0 to 1.5 ...
        Script b; b.length = at (2.0, sr);
        b.manual = { { 256 * 30, true }, { 256 * 40, false }, { 256 * 140, true }, { 256 * 150, false } };   // 0.16 s (low), 0.75 s (high)
        const auto r = play (b, sr, fixedBlock (256), s);
        auto e = edgesOf (r.trace);
        CHECK (e.size() == 3 && e[0] == 256 * 30 && e[1] == at (0.5, sr), "Trigger button: an edge while the clock is low, none while it is high (%d)", (int) e.size());
    }
    // Hold and track with the internal clock tracks during the high half.
    {
        Settings s; s.clock = clockInternal; s.rateHz = 2.0; s.mode = modeHoldTrack;
        Script b; b.length = at (1.0, sr);
        const auto r = play (b, sr, fixedBlock (512), s);
        int tr = 0;
        for (auto& t : r.trace) tr += t.tracking ? 1 : 0;
        CHECK (std::abs (tr - at (0.5, sr)) <= 2, "hold and track, internal 2 Hz: tracks for half of each tick (%d samples)", tr);
    }
    // A rate change keeps the phase: no extra tick.
    {
        Settings s; s.clock = clockInternal; s.rateHz = 2.0;
        Script b; b.length = at (3.0, sr);
        Settings s2 = s; s2.rateHz = 1.0;
        b.changes.push_back ({ 48128, s2 });   // a block boundary just after 1 s: the clock is half-way through a high
        const auto r = play (b, sr, fixedBlock (512), s);
        auto e = edgesOf (r.trace);
        bool noExtra = true;
        for (size_t k = 1; k < e.size(); ++k) noExtra = noExtra && e[k] - e[k - 1] >= at (0.49, sr);
        CHECK (noExtra && e.size() == 4, "rate halved mid-tick: no extra tick, the next comes later (%d ticks)", (int) e.size());
    }
}

// ------------------------------------------------------------------------------------------------ 5. notes

static void testNotes()
{
    const double sr = 48000.0;
    Settings s;
    auto edgesFor = [&] (Script sc) { sc.length = at (1.0, sr); return (int) edgesOf (play (sc, sr, fixedBlock (128), s).trace).size(); };
    { Script sc; sc.note (1000, 5000, 60); sc.note (1000, 5000, 64); sc.note (1000, 5000, 67); CHECK (edgesFor (sc) == 1, "a chord is one sample moment"); }
    { Script sc; sc.note (1000, 5000, 60); sc.note (4000, 5000, 62); CHECK (edgesFor (sc) == 1, "legato (overlapping) notes: one sample moment"); }
    { Script sc; sc.note (1000, 3000, 60); sc.note (4000, 3000, 62); CHECK (edgesFor (sc) == 2, "back-to-back notes (off and on at the same sample): two"); }
    { Script sc; sc.note (1000, 3000, 60); sc.note (4000, 3000, 60); CHECK (edgesFor (sc) == 2, "the same key back-to-back: two"); }
    { Script sc; sc.note (1000, 0, 60); CHECK (edgesFor (sc) == 1, "a zero-length note samples"); }
    {
        Script sc; sc.length = at (1.0, sr);
        sc.events.push_back ({ 1000, InputEvent::noteOn, 0, 60, 100 });
        sc.events.push_back ({ 2000, InputEvent::noteOn, 0, 60, 100 });   // pressed again, no release
        sc.events.push_back ({ 3000, InputEvent::noteOff, 0, 60, 0 });
        const auto r = play (sc, sr, fixedBlock (128), s);
        CHECK (! r.trace.back().gate && edgesOf (r.trace).size() == 1, "a key pressed twice: counted once, its first release lets it go");
    }
    {
        Script sc; sc.length = at (1.0, sr);
        sc.events.push_back ({ 1000, InputEvent::noteOn, 3, 60, 100 });
        sc.events.push_back ({ 1000, InputEvent::noteOn, 3, 64, 100 });
        sc.events.push_back ({ 5000, InputEvent::allNotesOff, 3, 0, 0 });
        const auto r = play (sc, sr, fixedBlock (128), s);
        CHECK (! r.trace[6000].gate, "All Notes Off releases that channel's keys");
    }
}

// ------------------------------------------------------------------------------------------------ 6. rest

static void testRest()
{
    const double sr = 48000.0;
    for (int sendAs : { sendAsCc, sendAsBoth })
        for (int mode : { modeSampleHold, modeHoldTrack })
        {
            Settings s; s.sendAs = sendAs; s.mode = mode; s.outOn[outNoise] = true; s.slew = 0.5;
            Script sc; sc.length = at (2.0, sr);
            sc.events.push_back ({ at (0.1, sr), InputEvent::noteOn, 1, 64, 100 });
            sc.rests.push_back (512 * 60);                                         // while held (0.64 s)
            sc.events.push_back ({ at (1.0, sr), InputEvent::noteOff, 1, 64, 0 });  // its later release
            sc.note (at (1.5, sr), at (0.1, sr), 70);
            const auto r = play (sc, sr, fixedBlock (512), s);
            Receiver rx;
            std::vector<Msg> before, atRest, after;
            for (auto& m : r.msgs) (m.at < 512 * 60 ? before : (m.at == 512 * 60 ? atRest : after)).push_back (m);
            rx.take (before);
            CHECK (rx.clockOn(), "rest test: the Clock gate is on before");
            rx.take (atRest);
            CHECK (rx.value[13][outSignal] == kMiddle14 && rx.value[13][outNoise] == kMiddle14, "rest: Signal and Noise to 0 V at the first sample");
            CHECK (! rx.clockOn(), "rest: the Clock gate ended at the first sample (%s)", sendAs == sendAsBoth ? "CC 0 and note-off" : "CC 0");
            CHECK (! r.trace[(size_t) (512 * 60)].gate && r.trace[(size_t) (512 * 60)].level == kMiddle14 / 16383.0, "rest: gate low, output at 0 V at once (no glide)");
            const long long rel = at (1.0, sr);
            bool changed = false;
            for (size_t i = (size_t) (512 * 60); i < (size_t) at (1.5, sr); ++i) changed = changed || r.trace[i].gate || r.trace[i].edge;
            CHECK (! changed, "rest: the key's later release does nothing (%lld)", rel);
            CHECK (r.trace[(size_t) at (1.5, sr)].edge, "rest: the next note samples");
            rx.take (after);
            CHECK (! rx.clockOn() && rx.doubled == 0 && rx.strayOff == 0, "rest: nothing left on at the end, nothing doubled");
        }
    // The internal clock after a rest: the rest is sent, the next tick half a tick later.
    {
        Settings s; s.clock = clockInternal; s.rateHz = 4.0;
        Script sc; sc.length = at (2.0, sr);
        sc.rests.push_back (512 * 50);
        const auto r = play (sc, sr, fixedBlock (512), s);
        const auto sig = values (r.msgs, outSignal);
        bool rest = false;
        for (auto& v : sig) if (v.first == 512 * 50 && v.second == kMiddle14) rest = true;
        long long next = -1;
        for (size_t i = 512 * 50; i < r.trace.size() && next < 0; ++i) if (r.trace[i].edge) next = (long long) i;
        CHECK (rest && next == 512 * 50 + at (0.125, sr), "internal clock: rest sent, next tick half a tick later");
    }
    // Bypass: rest once, then nothing while bypassed.
    {
        Settings s; s.clock = clockInternal; s.rateHz = 10.0; s.outOn[outNoise] = true;
        Script sc; sc.length = at (2.0, sr);
        sc.rests.push_back (512 * 40);
        sc.bypassFrom.push_back (512 * 40); sc.bypassTo.push_back (512 * 120);
        const auto r = play (sc, sr, fixedBlock (512), s, false);
        int during = 0, atStart = 0;
        for (auto& m : r.msgs) { if (m.at > 512 * 40 && m.at < 512 * 120) ++during; if (m.at == 512 * 40) ++atStart; }
        CHECK (during == 0 && atStart >= 3, "bypassed: rest sent once (%d messages), then nothing (%d)", atStart, during);
    }
}

// ------------------------------------------------------------------------------------------------ 7. control changes

static void testChanges()
{
    const double sr = 48000.0;
    // A key held (Notes clock), the Clock gate on as CC and note: change Send as, the channel, the switches, the mode,
    // the clock source, Level, the rectifier, the slew, the source: no new sample moment, no new gate.
    {
        Settings base; base.sendAs = sendAsBoth; base.outOn[outNoise] = true;
        Script sc; sc.length = at (6.0, sr);
        sc.events.push_back ({ at (0.1, sr), InputEvent::noteOn, 0, 60, 100 });
        std::mt19937 rng (3);
        for (int k = 1; k < 200; ++k)
        {
            Settings s = base;
            s.sendAs = (int) (rng() % 3);
            s.outChannel = 1 + (int) (rng() % 16);
            s.outOn[0] = rng() % 2; s.outOn[1] = rng() % 2; s.pulseOn[0] = rng() % 2;
            s.mode = (int) (rng() % 2);
            s.clock = (int) (rng() % 2) == 0 ? clockNotes : clockButton;
            s.level = (double) (rng() % 101) / 100.0; s.rectify = rng() % 2;
            s.slew = (rng() % 3) * 0.05; s.source = (int) (rng() % 5); s.sourceCc = (int) (rng() % 128);
            sc.changes.push_back ({ (long long) k * 1024 + 8192, s });
        }
        sc.changes.push_back ({ 210 * 1024 + 8192, base });
        sc.events.push_back ({ at (5.5, sr), InputEvent::noteOff, 0, 60, 0 });
        const auto r = play (sc, sr, fixedBlock (1024), base);
        CHECK (r.edges == 1, "200 control changes while a key is held: no new sample moment (%u)", r.edges);
        Receiver rx;
        int starts = 0;
        for (auto& m : r.msgs) { rx.take (m); if ((m.note && m.num == 48 && m.val > 0) || (! m.note && m.num == 79 && m.val == 127)) ++starts; }
        CHECK (starts == 2, "and no new Clock gate: only the note's own (CC and note) (%d)", starts);
        CHECK (! rx.clockOn() && rx.doubled == 0 && rx.strayOff == 0, "nothing left on, nothing doubled, no stray note-off");
        CHECK (overBlock (r) == 0, "at most one value per output per block through all the changes");
    }
    // Switching the clock to Notes while a key is held: waits for the next note.
    {
        Settings s; s.clock = clockButton;
        Script sc; sc.length = at (1.0, sr);
        sc.events.push_back ({ 100, InputEvent::noteOn, 0, 60, 100 });
        Settings s2 = s; s2.clock = clockNotes;
        sc.changes.push_back ({ 1024 * 10, s2 });
        sc.events.push_back ({ 1024 * 20, InputEvent::noteOff, 0, 60, 0 });
        sc.note (1024 * 30, 1000, 62);
        const auto r = play (sc, sr, fixedBlock (1024), s);
        auto e = edgesOf (r.trace);
        CHECK (e.size() == 1 && e[0] == 1024 * 30, "Clock switched to Notes with a key held: no sample until the next note");
    }
    // Switching to hold and track while the gate is high: waits for the next edge; back to sample and hold: holds.
    {
        Settings s; s.source = sourceCc; s.sourceCc = 20;
        Script sc; sc.length = at (1.0, sr);
        for (int k = 0; k < 90; ++k) sc.ccAt (512 * k, 20, k);
        sc.events.push_back ({ 600, InputEvent::noteOn, 0, 60, 100 });
        Settings s2 = s; s2.mode = modeHoldTrack;
        sc.changes.push_back ({ 512 * 10, s2 });
        const auto r = play (sc, sr, fixedBlock (512), s);
        CHECK (r.trace[512 * 30].level == r.trace[700].level, "switched to hold and track with a key held: holds until the next note");
    }
    // Outputs switched off send 0 V once, then nothing; the channel change sends 0 V on the old channel.
    {
        Settings s; s.clock = clockInternal; s.rateHz = 20.0; s.outOn[outNoise] = true;
        Script sc; sc.length = at (2.0, sr);
        Settings off = s; off.outOn[outSignal] = false; off.outOn[outNoise] = false;
        sc.changes.push_back ({ 512 * 40, off });
        Settings ch5 = s; ch5.outChannel = 5;
        sc.changes.push_back ({ 512 * 100, ch5 });
        const auto r = play (sc, sr, fixedBlock (512), s);
        int duringOff = 0; bool restSig = false, restNoise = false, oldCh = false, newCh = false;
        auto sigV = values (r.msgs, outSignal, 13);
        auto noiV = values (r.msgs, outNoise, 13);
        for (auto& v : sigV) { if (v.first == 512 * 40 && v.second == kMiddle14) restSig = true; if (v.first > 512 * 40 && v.first < 512 * 100) ++duringOff; }
        for (auto& v : noiV) { if (v.first == 512 * 40 && v.second == kMiddle14) restNoise = true; if (v.first > 512 * 40 && v.first < 512 * 100) ++duringOff; }
        for (auto& m : r.msgs) { if (m.at == 512 * 100 && m.ch == 13 && m.num == 76) oldCh = true; if (m.at >= 512 * 100 && m.ch == 5) newCh = true; }
        CHECK (restSig && restNoise && duringOff == 0, "outputs switched off: 0 V sent once, then nothing (%d)", duringOff);
        CHECK (newCh && ! oldCh, "outputs on again on channel 5: everything on channel 5 (the old channel was already at 0 V)");
        // The channel changed while Signal is away from 0 V: 0 V goes to the old channel first.
        Script sc2; sc2.length = at (1.0, sr);
        Settings c7 = s; c7.outChannel = 7;
        sc2.changes.push_back ({ 512 * 40, c7 });
        const auto r2 = play (sc2, sr, fixedBlock (512), s);
        Receiver rx; rx.take (r2.msgs);
        CHECK (rx.value[13][outSignal] == kMiddle14 && rx.value[13][outNoise] == kMiddle14 && rx.value[7][outSignal] >= 0, "channel change: 0 V on the old channel 13, then on channel 7");
    }
}

// ------------------------------------------------------------------------------------------------ 8. MIDI, noise, CPU

static void testMidi()
{
    // 40 Hz (the fastest), slew off, 44.1 kHz, buffer 1024: every step reaches the receiver at its own sample.
    {
        const double sr = 44100.0;
        Settings s; s.clock = clockInternal; s.rateHz = 40.0;
        Script sc; sc.length = at (5.0, sr);
        const auto r = play (sc, sr, fixedBlock (1024), s);
        const auto e = edgesOf (r.trace);
        const auto sig = values (r.msgs, outSignal);
        int reached = 0;
        for (auto x : e) for (auto& v : sig) if (v.first == x && v.second == toFourteenBit (r.trace[(size_t) x].level)) { ++reached; break; }
        CHECK (reached == (int) e.size() && e.size() >= 199, "40 Hz at 44.1 kHz, buffer 1024: all %d steps reach the receiver at their own sample (%d)", (int) e.size(), reached);
    }
    // The 6 ms and 0.2% rule while gliding, and nothing while it stands still.
    {
        const double sr = 48000.0;
        Settings s; s.source = sourcePitchBend; s.slew = 2.0;
        Script sc; sc.length = at (6.0, sr);
        sc.bendAt (0, 16383); sc.note (at (0.5, sr), 100);
        const auto r = play (sc, sr, fixedBlock (64), s);
        const auto sig = values (r.msgs, outSignal);
        int tooSoon = 0, during = 0, still = 0;
        for (size_t k = 1; k < sig.size(); ++k)
        {
            const long long gap = sig[k].first - sig[k - 1].first;
            const bool last = sig[k].second == 16383;
            if (! last && gap < at (0.006, sr)) ++tooSoon;
            if (! last && std::abs (sig[k].second - sig[k - 1].second) < 32 && gap < at (0.1, sr)) ++tooSoon;
            if (sig[k].first > at (0.5, sr) && sig[k].first <= at (2.5, sr)) ++during;
            if (sig[k].first > at (2.6, sr)) ++still;
        }
        CHECK (tooSoon == 0 && during > 30 && still == 0, "gliding: values at least 6 ms and 0.2%% apart (%d too soon), %d sent in a 2 s glide, none while still (%d)", tooSoon, during, still);
    }
    // Traffic, measured.
    double msgsSig = 0, msgsNoise = 0, msgsHt = 0;
    {
        const double sr = 48000.0;
        Settings s; s.clock = clockInternal; s.rateHz = 4.0; s.slew = 0.1;
        Script sc; sc.length = at (10.0, sr);
        auto r = play (sc, sr, fixedBlock (256), s, false);
        msgsSig = (double) r.msgs.size() / 10.0;
        s.slew = 0.0; s.outOn[outSignal] = false; s.pulseOn[0] = false; s.outOn[outNoise] = true;
        r = play (sc, sr, fixedBlock (256), s, false);
        msgsNoise = (double) r.msgs.size() / 10.0;
        Settings h; h.clock = clockInternal; h.rateHz = 1.0; h.mode = modeHoldTrack;
        r = play (sc, sr, fixedBlock (256), h, false);
        msgsHt = (double) r.msgs.size() / 10.0;
        CHECK (msgsSig < 200 && msgsNoise < 400 && msgsHt < 250, "traffic within bounds (%.0f, %.0f, %.0f per second)", msgsSig, msgsNoise, msgsHt);
    }
    std::printf ("MIDI: %.0f messages a second for a 4 Hz clock with a 100 ms slew (Signal and Clock); Noise output alone %.0f a second; hold and track on noise %.0f a second (buffer 256)\n",
                 msgsSig, msgsNoise, msgsHt);

    // The noise: evenly spread over -1 .. +1.
    {
        Noise n (99);
        int bins[10] {};
        double sum = 0, lo = 1, hi = -1;
        const int N = 2000000;
        for (int i = 0; i < N; ++i)
        {
            const double v = n.next();
            sum += v; lo = std::min (lo, v); hi = std::max (hi, v);
            ++bins[std::min (9, (int) ((v + 1.0) * 5.0))];
        }
        bool even = true;
        for (int b : bins) even = even && std::fabs ((double) b / N - 0.1) < 0.002;
        CHECK (even && std::fabs (sum / N) < 0.002 && lo < -0.9999 && hi > 0.9999 && hi < 1.0, "noise evenly spread over -1 .. +1 (mean %.4f)", sum / N);
        Noise a (5), b (5), c (6);
        bool same = true, differ = false;
        for (int i = 0; i < 1000; ++i) { const double x = a.next(), y = b.next(), z = c.next(); same = same && x == y; differ = differ || x != z; }
        CHECK (same && differ, "the same seed gives the same noise, another seed other noise");
    }

    // CPU: the heaviest settings (hold and track on noise with the slew restarting every sample, Noise output on,
    // internal clock 40 Hz, Clock as both), 60 s of audio at 48 kHz in 256-sample blocks.
    {
        Engine e;
        e.prepare (48000.0);
        Settings s; s.mode = modeHoldTrack; s.slew = 0.05; s.clock = clockInternal; s.rateHz = 40.0; s.outOn[outNoise] = true; s.sendAs = sendAsBoth;
        e.setSettings (s);
        struct Null final : MidiSink { long long n = 0; void cc (int, int, int, int) override { ++n; } void note (int, int, int, int) override { ++n; } } sink;
        const int blocks = 48000 * 60 / 256;
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < blocks; ++b) e.render (256, nullptr, 0, false, sink);
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        const double pct = secs / 60.0 * 100.0;
        CHECK (pct < 2.0, "CPU under 2%% of one core (%.3f%%)", pct);
        std::printf ("CPU: %.3f%% of one core at 48 kHz, heaviest settings (%.1f ns a sample)\n", pct, secs / (48000.0 * 60.0) * 1e9);
    }
}

// ------------------------------------------------------------------------------------------------ 9. fuzz

static void fuzz (unsigned seed)
{
    const double sr = 48000.0;
    Engine e;
    e.setSeed (seed);
    e.prepare (sr);
    e.panic();
    std::mt19937 rng (seed);
    Rec rec;
    Receiver rx;
    std::vector<InputEvent> buf;
    long long pos = 0, events = 0;
    bool down[16][128] {};
    int overs = 0;
    Settings s;
    for (int block = 0; block < 20000; ++block)
    {
        const int n = rng() % 40 == 0 ? (int) (rng() % 2) : 1 + (int) (rng() % 1024);
        if (rng() % 25 == 0)
        {
            s.mode = (int) (rng() % 2); s.clock = (int) (rng() % 3); s.rateHz = rateHz ((rng() % 101) / 100.0, rng() % 2);
            s.slew = (rng() % 4) * 0.03; s.level = (rng() % 101) / 100.0; s.rectify = rng() % 2; s.source = (int) (rng() % 5);
            s.sourceCc = (int) (rng() % 128); s.outChannel = 1 + (int) (rng() % 3); s.sendAs = (int) (rng() % 3);
            s.outOn[0] = rng() % 4 != 0; s.outOn[1] = rng() % 3 == 0; s.pulseOn[0] = rng() % 4 != 0;
            e.setSettings (s);
        }
        if (rng() % 300 == 0) e.panic();
        buf.clear();
        const int count = (int) (rng() % 6);
        std::vector<int> offs;
        for (int k = 0; k < count; ++k) offs.push_back (n > 0 ? (int) (rng() % (unsigned) n) : 0);
        std::sort (offs.begin(), offs.end());
        for (int k = 0; k < count; ++k)
        {
            const int ch = (int) (rng() % 3), nt = 58 + (int) (rng() % 6);
            const unsigned kind = rng() % 24;
            InputEvent ev { offs[(size_t) k], InputEvent::noteOn, (std::uint8_t) ch, (std::uint8_t) nt, 1 + (int) (rng() % 127) };
            if (kind == 0)      { ev.kind = InputEvent::allNotesOff; for (auto& d : down[ch]) d = false; }
            else if (kind < 4)  { ev.kind = InputEvent::controller; ev.number = (std::uint8_t) (rng() % 128); ev.value = (int) (rng() % 128); }
            else if (kind < 5)  { ev.kind = InputEvent::pitchBend; ev.value = (int) (rng() % 16384); }
            else if (kind < 6)  { ev.kind = InputEvent::pressure; ev.value = (int) (rng() % 128); }
            else if (kind < 15) down[ch][nt] = true;
            else                { ev.kind = InputEvent::noteOff; down[ch][nt] = false; }
            buf.push_back (ev);
            ++events;
        }
        const size_t before = rec.m.size();
        rec.base = pos;
        e.render (n, buf.data(), (int) buf.size(), rng() % 50 == 0, rec);
        std::map<std::pair<int, int>, int> per;
        for (size_t k = before; k < rec.m.size(); ++k) if (! rec.m[k].note && (rec.m[k].num == 76 || rec.m[k].num == 78)) ++per[{ rec.m[k].ch, rec.m[k].num }];
        for (auto& [key, c] : per) overs += c > 1 ? 1 : 0;
        pos += n;
    }
    // Release every key, then wait.
    buf.clear();
    for (int ch = 0; ch < 16; ++ch) for (int nt = 0; nt < 128; ++nt) if (down[ch][nt]) buf.push_back ({ 0, InputEvent::noteOff, (std::uint8_t) ch, (std::uint8_t) nt, 0 });
    rec.base = pos;
    e.render (512, buf.data(), (int) buf.size(), false, rec);
    pos += 512;
    for (int k = 0; k < 400; ++k) { rec.base = pos; e.render (512, nullptr, 0, false, rec); pos += 512; }
    rx.take (rec.m);
    const bool gateLow = ! e.gate() || e.settings().clock == clockInternal;
    CHECK (gateLow, "fuzz %u: after every key is released the clock gate is low (Notes / Button)", seed);
    // Then a rest: everything at 0 V on every channel used, nothing on.
    e.panic();
    Settings fin = s; fin.clock = clockNotes; fin.outOn[0] = fin.outOn[1] = true;
    e.setSettings (fin);
    rec.m.clear();
    for (int k = 0; k < 4; ++k) { rec.base = pos; e.render (512, nullptr, 0, false, rec); pos += 512; }
    rx.take (rec.m);
    const int restSig = fin.rectify ? 0 : kMiddle14;
    bool atRest = true;
    for (int ch = 1; ch <= 16; ++ch)
    {
        if (rx.value[ch][0] >= 0 && ch != fin.outChannel && rx.value[ch][0] != kMiddle14 && rx.value[ch][0] != 0)
        { atRest = false; std::printf ("  channel %d Signal %d\n", ch, rx.value[ch][0]); }   // old channels got 0 V (middle, or bottom if rectified then)
        if (rx.value[ch][1] >= 0 && rx.value[ch][1] != kMiddle14 && ch != fin.outChannel)   // on the out channel the noise runs on
        {
            atRest = false; std::printf ("  channel %d Noise %d\n", ch, rx.value[ch][1]);
        }
    }
    bool noiseRest = false;
    for (auto& v : values (rec.m, outNoise, fin.outChannel)) if (v.first == pos - 4 * 512 && v.second == kMiddle14) noiseRest = true;
    if (! noiseRest) { atRest = false; std::printf ("  out channel %d: Noise not sent to 0 V at the rest\n", fin.outChannel); }
    if (rx.value[fin.outChannel][0] != restSig) { atRest = false; std::printf ("  out channel %d Signal %d (want %d)\n", fin.outChannel, rx.value[fin.outChannel][0], restSig); }
    CHECK (atRest, "fuzz %u: after a rest every channel's receiver is at 0 V (the Noise output sends 0 V, then runs on)", seed);
    CHECK (! rx.clockOn() && rx.doubled == 0 && rx.strayOff == 0, "fuzz %u: no Clock CC or note left on, none doubled, no stray note-off", seed);
    CHECK (overs == 0, "fuzz %u: never more than one value per output per block (%d)", seed, overs);
    std::printf ("stuck-gate and stuck-note fuzz: %lld random events in 20,000 blocks (seed %u), nothing stuck\n", events, seed);
}

int main()
{
    testDefaults();
    testTiming();
    testSignal();
    testClock();
    testNotes();
    testRest();
    testChanges();
    testMidi();
    fuzz (777);
    fuzz (4242);
    std::printf ("engine tests: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
