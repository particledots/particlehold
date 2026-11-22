// The whole particlehold engine: notes and controllers in, the held value, the noise and the clock out as MIDI,
// sample-accurate. Plain C++, no plugin code, so it is tested without Logic (engine/tests). Built the same way as
// Bryn 0.1.0's Engine (its Keys, Curve and CcStream are used as they are; Pulses adapted).
//
// Per sample:
//   1. The incoming events at this sample are applied: keys (the gate from notes) and the source values (the chosen
//      controller, aftertouch, pitch bend, the latest velocity).
//   2. The clock gate: the internal clock, the held notes, or neither (Button only), together with the Trigger button.
//   3. The input: the source (the noise, or the chosen MIDI value, read with its middle as 0 V) x Level, then the
//      rectifier. On the 0..1 output scale 0 V is the middle (rectifier off) or the bottom (rectifier on).
//   4. Sample and hold: a new value at every rising edge of the clock gate. Hold and track: follows the input while
//      the gate is high, holds the value it had when the gate fell.
//   5. The slew, then the outputs: Signal and Noise as 14-bit CC (CcStream), the Clock gate as CC and/or a note.
// It never touches the incoming MIDI: passing everything through is the plugin's job.

#pragma once

#include "particlehold/CcStream.h"
#include "particlehold/Clock.h"
#include "particlehold/Glide.h"
#include "particlehold/Keys.h"
#include "particlehold/Noise.h"
#include "particlehold/Outputs.h"
#include "particlehold/Pulses.h"

#include <cstdint>
#include <vector>

namespace particlehold
{
    struct InputEvent
    {
        enum Kind : std::uint8_t { noteOn, noteOff, allNotesOff, controller, pitchBend, pressure };
        int offset;            // sample within the block
        Kind kind;
        std::uint8_t channel;  // 0..15
        std::uint8_t number;   // note or controller number (ignored for allNotesOff, pitchBend, pressure)
        int value = 0;         // velocity (noteOn), controller value 0..127, pitch bend 0..16383, pressure 0..127
    };

    enum Mode   { modeSampleHold = 0, modeHoldTrack = 1 };                                       // the Mode menu
    enum ClockSource { clockInternal = 0, clockNotes = 1, clockButton = 2 };                     // the Clock menu
    enum Source { sourceNoise = 0, sourceCc = 1, sourceAftertouch = 2, sourcePitchBend = 3, sourceVelocity = 4 };   // the Source menu

    struct Settings
    {
        int    mode = modeSampleHold;
        double level = 1.0;           // 0 .. 1
        bool   rectify = false;
        double slew = 0.0;            // seconds; 0 = off
        double rateHz = 0.447;        // the internal clock (Rate and Range worked out by the plugin)
        int    clock = clockNotes;
        int    source = sourceNoise;
        int    sourceCc = 1;          // 0..127
        int    outChannel = kDefaultChannel;                       // 1..16
        bool   outOn[kNumSmoothOutputs] = { true, false };         // Signal, Noise
        bool   pulseOn[kNumPulseOutputs] = { true };               // Clock
        int    sendAs = sendAsCc;
    };

    // One sample, for the tests.
    struct TracePoint { double level, input; bool gate, edge, corner, tracking; };

    // A point of the panel's scope (every 10 ms).
    struct ScopePoint { float input, output; bool edge, gate; };

    class Engine
    {
    public:
        Engine();

        void prepare (double sampleRate);         // also goes to rest and sends everything afresh
        void setSettings (const Settings& s);     // applied at the start of the next block
        const Settings& settings() const { return pending; }
        void setSeed (std::uint64_t seed) { noise.setSeed (seed); }

        // Events must be in sample order. manualGate = the panel's Trigger button (applied at the block's first sample).
        void render (int numSamples, const InputEvent* events, int numEvents, bool manualGate, MidiSink& sink);

        // Straight to rest at the start of the next rendered block (or of flushOnly): Signal and Noise to 0 V (sent if
        // not already there), every key released, the Trigger button forgotten until it is let go, the Clock gate
        // ended, the internal clock restarted. Used for transport stop, reset, bypass.
        void panic();

        // For the bypassed path: no events are read and nothing moves; a pending rest is still sent.
        void flushOnly (int numSamples, MidiSink& sink);

        // What the panel shows.
        double output() const       { return glide.level(); }   // 0 .. 1 (the Signal output before 14-bit)
        double input() const        { return inputNow; }        // 0 .. 1 (the input after Level and the rectifier)
        bool gate() const           { return gateNow; }
        bool tracking() const       { return trackingNow; }
        bool gliding() const        { return glide.isGliding(); }
        unsigned edges() const      { return edgeCount; }       // sample moments so far
        bool rectified() const      { return active.rectify; }
        int heldKeys() const        { return keys.held(); }
        const CcStream& ccStream() const { return cc; }
        const Pulses& pulses() const { return pulse; }
        long long samplesDone() const { return abs; }

        // The scope: a point every 10 ms, the last kScopeSize kept. scopeCount() counts every point ever made.
        static constexpr int kScopeSize = 512;
        const ScopePoint& scopePoint (unsigned long long index) const { return scope[index % kScopeSize]; }
        unsigned long long scopeCount() const { return scopeMade; }

        // Tests only: when set, every sample is appended here.
        std::vector<TracePoint>* trace = nullptr;

    private:
        // Everything the engine sends in a block is collected here first, then handed on in sample order. Never
        // allocates while running: the space is reserved once. (As Bryn's.)
        struct Queue final : MidiSink
        {
            struct Item { int offset, channel, number, value; bool note; };
            std::vector<Item> items;
            void cc (int o, int ch, int n, int v) override   { items.push_back ({ o, ch, n, v, false }); }
            void note (int o, int ch, int n, int v) override { items.push_back ({ o, ch, n, v, true }); }
            void handOn (MidiSink& sink);
        };

        void applySettings (MidiSink& sink);
        void restNow (bool manualGate, MidiSink& sink);
        void apply (const InputEvent& ev);
        double sourceValue (double noiseNow) const;   // -1 .. +1
        double restLevel() const { return active.rectify ? 0.0 : (double) kMiddle14 / 16383.0; }
        int restSignal14() const { return active.rectify ? 0 : kMiddle14; }

        Settings pending, active;
        bool settingsChanged = true;
        Keys keys;
        Clock clock;
        Glide glide;
        Noise noise;
        CcStream cc;
        Pulses pulse;
        Queue queue;

        // The source values, as 14-bit (0..16383).
        int ccValue[128] {};          // the latest value of every controller, any channel (14-bit where a pair)
        int ccCoarse[128] {};
        int bendValue = 8192, pressureValue = 0, velocityValue = 0;

        long long abs = 0;
        double inputNow = 0.5;
        bool gateNow = false, trackingNow = false;
        bool manualHeld = false, manualBlocked = false;   // after a panic the button must be let go first
        bool sourceBlocked = false;                       // after a Clock change: the new source counts once it is low
        bool trackBlocked = false;                        // after a Mode change: hold and track waits for a new edge
        bool panicPending = false, pulsesToClose = false;
        bool blipPending = false;                         // a note-on in a block of no samples
        unsigned edgeCount = 0;
        double sampleRate = 48000.0;

        long long scopeStep = 480, scopeIn = 0;
        bool scopeEdge = false;
        ScopePoint scope[kScopeSize] {};
        unsigned long long scopeMade = 0;
    };
}
