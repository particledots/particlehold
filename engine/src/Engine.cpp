// The particlehold engine. Built the same way as Bryn 0.1.0's Engine (engine/src/Engine.cpp): the same block
// structure, rest, key handling and MIDI queue; the sample and hold, the sources, the clock and the slew are new.
#include "particlehold/Engine.h"

#include <cmath>
#include <utility>

namespace particlehold
{
    namespace
    {
        // The fine half of a 14-bit pair, for a controller used as the source: the standard pairs (CC 0-31 with
        // 32-63) and the particledots direct pairs (Bryn 80 / 81, Strange Drift 106-112 / 113-119, particlehold
        // 75 / 76 and 77 / 78). Hillside's and Ogdoad's pairs are standard ones. -1: none.
        int coarseOfFine (int fine)
        {
            if (fine >= 32 && fine <= 63) return fine - 32;
            if (fine == 81) return 80;
            if (fine >= 113 && fine <= 119) return fine - 7;
            if (fine == 76) return 75;
            if (fine == 78) return 77;
            return -1;
        }

        // A 7-bit value on the 14-bit scale, so 0 is the bottom, 127 the very top, and the coarse part stays the
        // same (v * 129 = v * 128 + v).
        int full14 (int v7) { return (v7 < 0 ? 0 : (v7 > 127 ? 127 : v7)) * 129; }
    }

    Engine::Engine()
    {
        for (int i = 0; i < kNumSmoothOutputs; ++i) cc.setLane (i, kCoarseCc[i], kFineCc[i]);
        cc.setRest (outSignal, kMiddle14);
        cc.setRest (outNoise, kMiddle14);
        cc.setEnabled (outNoise, false, 0, 0);   // off by default (as the settings)
        queue.items.reserve (4096);
        prepare (48000.0);
    }

    void Engine::prepare (double sr)
    {
        sampleRate = sr > 0.0 ? sr : 48000.0;
        clock.prepare (sampleRate);
        clock.setRate (active.rateHz);
        clock.restart();
        glide.prepare (sampleRate);
        glide.setTime (active.slew);
        glide.jump (restLevel());
        cc.prepare (sampleRate);
        pulse.prepare (sampleRate);
        pulsesToClose = pulse.anyOn();   // a clock note left on is ended at the start of the next block
        keys.clear();
        abs = 0;
        gateNow = trackingNow = false;
        manualHeld = manualBlocked = false;
        sourceBlocked = trackBlocked = blipPending = false;
        panicPending = false;
        settingsChanged = true;
        inputNow = restLevel();
        scopeStep = (long long) std::llround (sampleRate / 100.0);
        if (scopeStep < 1) scopeStep = 1;
        scopeIn = 0;
        scopeEdge = false;
    }

    void Engine::setSettings (const Settings& s)
    {
        pending = s;
        if (pending.outChannel < 1) pending.outChannel = 1;
        if (pending.outChannel > 16) pending.outChannel = 16;
        if (pending.sendAs < sendAsCc || pending.sendAs > sendAsBoth) pending.sendAs = sendAsCc;
        if (pending.mode != modeHoldTrack) pending.mode = modeSampleHold;
        if (pending.clock < clockInternal || pending.clock > clockButton) pending.clock = clockNotes;
        if (pending.source < sourceNoise || pending.source > sourceVelocity) pending.source = sourceNoise;
        if (pending.sourceCc < 0) pending.sourceCc = 0;
        if (pending.sourceCc > 127) pending.sourceCc = 127;
        pending.level = pending.level < 0.0 ? 0.0 : (pending.level > 1.0 ? 1.0 : pending.level);
        if (pending.slew < kSlewMinSeconds * 0.5) pending.slew = 0.0;
        if (pending.slew > kSlewMaxSeconds) pending.slew = kSlewMaxSeconds;
        settingsChanged = true;
    }

    void Engine::panic() { panicPending = true; }

    void Engine::Queue::handOn (MidiSink& sink)
    {
        // Stable insertion sort by sample (the list is short and nearly in order already).
        for (std::size_t a = 1; a < items.size(); ++a)
            for (std::size_t b = a; b > 0 && items[b].offset < items[b - 1].offset; --b)
                std::swap (items[b], items[b - 1]);
        for (const auto& it : items)
        {
            if (it.note) sink.note (it.offset, it.channel, it.number, it.value);
            else         sink.cc (it.offset, it.channel, it.number, it.value);
        }
        items.clear();
    }

    void Engine::applySettings (MidiSink& sink)
    {
        settingsChanged = false;
        if (pending.outChannel != active.outChannel)
            cc.moveChannel (active.outChannel, 0, sink);   // 0 V (as it was) on the old channel
        cc.setRest (outSignal, pending.rectify ? 0 : kMiddle14);
        for (int i = 0; i < kNumSmoothOutputs; ++i)
            if (pending.outOn[i] != cc.isEnabled (i))
                cc.setEnabled (i, pending.outOn[i], abs, 0);
        pulse.configure (pending.pulseOn, pending.outChannel, pending.sendAs, 0, sink);

        // A new clock source is taken as it stands: if it is high now, it counts once it has been low (changing a
        // control never makes a sample moment or starts the Clock gate).
        if (pending.clock != active.clock)
        {
            const bool highNow = pending.clock == clockNotes ? keys.held() > 0
                               : (pending.clock == clockInternal ? clock.phase() < 0.5 : false);
            sourceBlocked = highNow;
        }
        // A new mode: hold and track only starts tracking at the next rising edge; leaving it holds where it is.
        if (pending.mode != active.mode)
        {
            trackBlocked = gateNow;
            if (trackingNow && pending.mode == modeSampleHold)
            {
                glide.setTarget (glide.goal(), true);
                trackingNow = false;
            }
        }
        glide.setTime (pending.slew);
        clock.setRate (pending.rateHz);
        active = pending;
    }

    void Engine::restNow (bool manualGate, MidiSink& sink)
    {
        panicPending = false;
        keys.clear();
        gateNow = trackingNow = false;
        trackBlocked = sourceBlocked = blipPending = false;
        manualBlocked = manualGate;   // a button held through a stop must be let go and pressed again
        clock.restart();
        glide.jump (restLevel());
        inputNow = restLevel();
        pulse.closeAll (0, sink);
        cc.force (outSignal, abs, 0, restSignal14());
        cc.force (outNoise, abs, 0, kMiddle14);
    }

    void Engine::flushOnly (int numSamples, MidiSink& sink)
    {
        queue.items.clear();
        if (pulsesToClose) { pulse.closeAll (0, queue); pulsesToClose = false; }
        if (settingsChanged) applySettings (queue);
        if (panicPending) restNow (manualHeld, queue);
        cc.flush (active.outChannel, queue);
        queue.handOn (sink);
        if (numSamples > 0) abs += numSamples;
    }

    void Engine::apply (const InputEvent& ev)
    {
        switch (ev.kind)
        {
            case InputEvent::noteOn:
                keys.press (ev.channel, ev.number);
                velocityValue = full14 (ev.value);
                break;
            case InputEvent::noteOff:     keys.release (ev.channel, ev.number); break;
            case InputEvent::allNotesOff: keys.releaseChannel (ev.channel); break;
            case InputEvent::controller:
            {
                const int n = ev.number & 127, v = ev.value < 0 ? 0 : (ev.value > 127 ? 127 : ev.value);
                ccCoarse[n] = v;
                ccValue[n] = full14 (v);                       // a coarse part (or a plain 7-bit controller)
                const int c = coarseOfFine (n);
                if (c >= 0) ccValue[c] = ccCoarse[c] * 128 + v;  // the fine part completes its pair
                break;
            }
            case InputEvent::pitchBend: bendValue = ev.value < 0 ? 0 : (ev.value > 16383 ? 16383 : ev.value); break;
            case InputEvent::pressure:  pressureValue = full14 (ev.value); break;
        }
    }

    double Engine::sourceValue (double noiseNow) const
    {
        int v14 = 8192;
        switch (active.source)
        {
            case sourceNoise:      return noiseNow;
            case sourceCc:         v14 = ccValue[active.sourceCc]; break;
            case sourceAftertouch: v14 = pressureValue; break;
            case sourcePitchBend:  v14 = bendValue; break;
            case sourceVelocity:   v14 = velocityValue; break;
            default: break;
        }
        return (double) v14 * (2.0 / 16383.0) - 1.0;
    }

    void Engine::render (int numSamples, const InputEvent* events, int numEvents, bool manualGate, MidiSink& sink)
    {
        queue.items.clear();
        if (pulsesToClose) { pulse.closeAll (0, queue); pulsesToClose = false; }
        if (settingsChanged) applySettings (queue);
        manualHeld = manualGate;
        if (panicPending) restNow (manualGate, queue);
        if (! manualGate) manualBlocked = false;
        const bool manual = manualGate && ! manualBlocked;

        if (numSamples <= 0)
        {   // a block of no samples: its events still count (a release must never be lost; a press counts at the
            // next sample even if released by then)
            if (numEvents > 0)
            {
                keys.beginSample (abs);
                for (int k = 0; k < numEvents; ++k)
                {
                    apply (events[k]);
                    if (events[k].kind == InputEvent::noteOn) blipPending = true;
                }
            }
            cc.flush (active.outChannel, queue);
            queue.handOn (sink);
            return;
        }

        int e = 0;
        const int last = numSamples - 1;
        for (int i = 0; i < numSamples; ++i)
        {
            bool keyEvents = false;
            if (e < numEvents && (events[e].offset <= i || i == last))
            {
                keys.beginSample (abs);
                while (e < numEvents && (events[e].offset <= i || i == last))
                {
                    const auto& ev = events[e++];
                    apply (ev);
                    keyEvents = keyEvents || ev.kind == InputEvent::noteOn || ev.kind == InputEvent::noteOff
                                          || ev.kind == InputEvent::allNotesOff;
                }
            }

            const double nz = noise.next();
            const bool internalHigh = clock.next();

            // The clock gate.
            bool notesHigh = keys.held() > 0;
            const bool backToBack = keyEvents && notesHigh && keys.backToBack();
            if (! notesHigh && ((keyEvents && keys.blip()) || blipPending)) notesHigh = true;   // a zero-length note: one sample
            blipPending = false;
            bool srcHigh = false, srcRetrigger = false;
            if (active.clock == clockInternal)   srcHigh = internalHigh;
            else if (active.clock == clockNotes) { srcHigh = notesHigh; srcRetrigger = backToBack; }
            if (sourceBlocked)
            {
                if (! srcHigh) sourceBlocked = false;
                else { srcHigh = false; srcRetrigger = false; }
            }
            const bool high = srcHigh || manual;
            const bool rose = (high && ! gateNow) || (srcRetrigger && ! manual && gateNow && high);
            gateNow = high;
            if (rose) { ++edgeCount; scopeEdge = true; }

            // The input: source x Level, then the rectifier, on the 0..1 output scale.
            const double y = sourceValue (nz) * active.level;
            const double u = active.rectify ? std::fabs (y) : (y + 1.0) * 0.5;
            inputNow = u;

            // Sample and hold, or hold and track.
            if (! gateNow) trackBlocked = false;
            if (active.mode == modeSampleHold)
            {
                if (rose) glide.setTarget (u, true);
            }
            else
            {
                if (rose) trackBlocked = false;
                const bool tr = gateNow && ! trackBlocked;
                if (tr)               glide.setTarget (u, rose);
                else if (trackingNow) glide.setTarget (glide.goal(), true);   // locks: the last tracked value, sent exactly
                trackingNow = tr;
            }

            const Glide::Out g = glide.next();
            pulse.step (abs, i, gateNow, rose, queue);
            cc.consider (outSignal, abs, i, toFourteenBit (g.level), g.corner);
            cc.consider (outNoise, abs, i, toFourteenBit ((nz + 1.0) * 0.5), false);

            if (++scopeIn >= scopeStep)
            {
                scopeIn = 0;
                scope[scopeMade % kScopeSize] = { (float) u, (float) g.level, scopeEdge, gateNow };
                ++scopeMade;
                scopeEdge = false;
            }
            if (trace != nullptr) trace->push_back ({ g.level, u, gateNow, rose, g.corner, trackingNow });
            ++abs;
        }
        cc.flush (active.outChannel, queue);
        queue.handOn (sink);
    }
}
