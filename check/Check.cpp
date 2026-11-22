// particleholdCheck: runs the plugin layer without a host. The build workflow runs it on every push and fails if any
// check fails. Adapted from Bryn's check.
//
//   1. The control list is exactly what 0.1.0 shipped (ids, order, ranges, defaults, menu lists): golden file
//      layout-<version>.txt for every version that shipped one.
//   2. Projects saved by every earlier version (golden saved states, every control away from its default) load
//      unchanged; the Trigger button never opens held.
//   3. It is a MIDI effect: no audio buses, MIDI in and out, no latency, no tail, 15 controls, the defaults.
//   4. The MIDI path: everything passes unchanged and in order (notes keep channel, velocity and sample); Signal goes
//      out on channel 13 as CC 75 / 76 at the note's sample, Noise as CC 77 / 78, the Clock as CC 79, note 48 or
//      both; 0 V sent at the start; every source read from the real MIDI (pitch bend, 14-bit CC, aftertouch).
//   5. Automation: every control moved through its range, with and without a held note: never more than one value
//      per output per block, no bursts from the Clock, and changing Send as, the channel or a switch never starts a
//      note or a gate.
//   6. Transport stop, reset / releaseResources / prepareToPlay, All Notes Off, bypass and a long pause in Logic's
//      calls while a note is held (Clock sent as CC and as notes): 0 V and the Clock's end sent at once, the key's
//      later release does nothing, the next note plays.
//   7. Stuck-gate and stuck-note fuzz through the real plugin: random notes, controllers, block sizes (including 0 and
//      1), control changes (all of them), stops, resets and bypass. After every key is released: the clock low, no
//      Clock note or CC left on; after a rest every channel at 0 V.
//   8. The panel: opens at its design size, draws, resizes between 80% and 200% with locked proportions, every
//      control inside the window and moving only its own parameter, the Trigger button samples, the lights and the
//      scope follow the engine. --snapshot <folder> writes pictures of it.

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Parameters.h"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <vector>

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

static constexpr int kNumControls = 15;
static const char* const kVersions[] = { "0.1.0" };   // every version that shipped a golden list and state
static constexpr int kMiddle = particlehold::kMiddle14;

static std::vector<juce::RangedAudioParameter*> allParams (ParticleholdProcessor& proc)
{
    std::vector<juce::RangedAudioParameter*> v;
    for (auto* base : proc.getParameters())
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (base)) v.push_back (p);
    return v;
}
static juce::RangedAudioParameter* P (ParticleholdProcessor& proc, const char* id)
{
    return dynamic_cast<juce::RangedAudioParameter*> (proc.getValueTreeState().getParameter (id));
}
static void setReal (ParticleholdProcessor& proc, const char* id, float v)
{
    auto* p = P (proc, id);
    p->setValueNotifyingHost (p->convertTo0to1 (v));
}

// A play head we can start and stop.
struct FakePlayHead final : juce::AudioPlayHead
{
    bool playing = false;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (playing);
        return info;
    }
};

struct Out { int pos; juce::MidiMessage m; };

static std::vector<Out> run (ParticleholdProcessor& proc, juce::MidiBuffer midi, int numSamples, bool bypassed = false)
{
    juce::AudioBuffer<float> audio (0, numSamples);
    if (bypassed) proc.processBlockBypassed (audio, midi);
    else          proc.processBlock (audio, midi);
    std::vector<Out> v;
    for (const auto meta : midi) v.push_back ({ meta.samplePosition, meta.getMessage() });
    return v;
}
static std::vector<Out> runEmpty (ParticleholdProcessor& proc, int blocks, int n = 512)
{
    std::vector<Out> all;
    for (int i = 0; i < blocks; ++i) for (auto& o : run (proc, {}, n)) all.push_back (o);
    return all;
}

static bool isOurCc (const juce::MidiMessage& m)
{
    if (! m.isController()) return false;
    const int n = m.getControllerNumber();
    return n >= 75 && n <= 79;
}
static bool isClockNote (const juce::MidiMessage& m) { return m.isNoteOnOrOff() && m.getNoteNumber() == 48; }

// What a mapper on each channel knows: the latest 14-bit Signal (CC 75 + 76) and Noise (CC 77 + 78).
struct Receiver
{
    int coarse[17][2] {};
    int value[17][2];
    Receiver() { for (auto& c : value) c[0] = c[1] = -1; }
    void take (const juce::MidiMessage& m)
    {
        if (! m.isController()) return;
        const int ch = m.getChannel(), n = m.getControllerNumber();
        if (n == 75) coarse[ch][0] = m.getControllerValue();
        if (n == 76) value[ch][0] = coarse[ch][0] * 128 + m.getControllerValue();
        if (n == 77) coarse[ch][1] = m.getControllerValue();
        if (n == 78) value[ch][1] = coarse[ch][1] * 128 + m.getControllerValue();
    }
    void take (const std::vector<Out>& v) { for (auto& o : v) take (o.m); }
};

// The Clock as a receiver sees it, per channel: CC 79 and note 48. Notes from the player use other note numbers.
struct ClockWatch
{
    int ccVal[17] {};
    bool noteOn[17] {};
    int doubled = 0, strayOff = 0, ccRepeat = 0, starts = 0;
    void take (const juce::MidiMessage& m)
    {
        const int ch = m.getChannel();
        if (m.isController() && m.getControllerNumber() == 79)
        {
            const int v = m.getControllerValue();
            ccRepeat += ccVal[ch] == v ? 1 : 0;
            starts += v == 127 ? 1 : 0;
            ccVal[ch] = v;
        }
        if (isClockNote (m))
        {
            if (m.isNoteOn()) { doubled += noteOn[ch] ? 1 : 0; noteOn[ch] = true; ++starts; }
            else              { strayOff += noteOn[ch] ? 0 : 1; noteOn[ch] = false; }
        }
    }
    void take (const std::vector<Out>& v) { for (auto& o : v) take (o.m); }
    bool anythingOn() const
    {
        for (int ch = 0; ch <= 16; ++ch) if (ccVal[ch] != 0 || noteOn[ch]) return true;
        return false;
    }
};

static void prepare (ParticleholdProcessor& proc, double sr = 48000.0, int block = 512)
{
    proc.setRateAndBufferSizeDetails (sr, block);
    proc.prepareToPlay (sr, block);
}
static juce::MidiBuffer one (const juce::MidiMessage& m, int pos = 0) { juce::MidiBuffer b; b.addEvent (m, pos); return b; }
static unsigned edges (const ParticleholdProcessor& p) { return p.getDisplay().edges.load(); }

// ------------------------------------------------------------------------------------------------ 1. control list

static juce::StringArray dumpLayout (ParticleholdProcessor& proc)
{
    juce::StringArray lines;
    int i = 0;
    for (auto* p : allParams (proc))
    {
        const auto& r = p->getNormalisableRange();
        juce::String line;
        line << i++ << " " << p->paramID << " v" << p->getVersionHint() << " | " << p->getName (100)
             << " | range " << juce::String (r.start, 3) << ".." << juce::String (r.end, 3) << " step " << juce::String (r.interval, 3)
             << " | default " << juce::String (p->convertFrom0to1 (p->getDefaultValue()), 3)
             << " | centre " << juce::String (p->convertFrom0to1 (0.5f), 3);
        if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (p)) line << " | choices " << c->choices.joinIntoString (";");
        if (dynamic_cast<juce::AudioParameterBool*> (p) != nullptr) line << " | switch";
        lines.add (line);
    }
    return lines;
}

// The normalised value every control gets in a golden project: away from its default. Each version its own pattern.
static float goldenValue (juce::RangedAudioParameter* p, int i, const juce::String& version)
{
    const int n = juce::jmax (2, p->getNumSteps() < 1000 ? p->getNumSteps() : 10);
    int k = version == "0.1.0" ? (i * 7 + 3) % n : (i * 5 + 2) % n;
    float v = (float) k / (float) (n - 1);
    if (std::abs (v - p->getDefaultValue()) < 1.0e-3f) v = (float) ((k + 1) % n) / (float) (n - 1);
    return p->convertTo0to1 (p->convertFrom0to1 (v));
}

static void testControlList (const juce::File& goldenDir, bool writeGolden)
{
    ParticleholdProcessor proc;
    const auto now = dumpLayout (proc);
    if (writeGolden)
    {   // writes the golden files of THIS version only, and never overwrites a version that already shipped
        const juce::String version (JucePlugin_VersionString);
        const juce::File golden = goldenDir.getChildFile ("layout-" + version + ".txt");
        const juce::File state  = goldenDir.getChildFile ("state-" + version + ".xml");
        if (golden.existsAsFile() || state.existsAsFile()) { std::printf ("golden files for %s already exist: not touched\n", version.toRawUTF8()); return; }
        golden.replaceWithText (now.joinIntoString ("\n") + "\n");
        auto params = allParams (proc);
        for (int i = 0; i < (int) params.size(); ++i) params[(size_t) i]->setValueNotifyingHost (goldenValue (params[(size_t) i], i, version));
        juce::MemoryBlock mb;
        proc.getStateInformation (mb);
        auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        state.replaceWithText (xml->toString());
        std::printf ("wrote %s (%d controls) and %s\n", golden.getFullPathName().toRawUTF8(), now.size(), state.getFullPathName().toRawUTF8());
        return;
    }
    for (const char* version : kVersions)
    {
        juce::StringArray old;
        old.addLines (goldenDir.getChildFile (juce::String ("layout-") + version + ".txt").loadFileAsString());
        old.removeEmptyStrings();
        CHECK (old.size() == kNumControls, "reference list %s has %d controls", version, old.size());
        CHECK (now.size() >= old.size(), "control count %d is smaller than %s's %d", now.size(), version, old.size());
        int same = 0;
        for (int i = 0; i < old.size() && i < now.size(); ++i)
        {
            if (old[i] == now[i]) ++same;
            else std::printf ("control %d differs from %s:\n   shipped: %s\n   now:     %s\n", i, version, old[i].toRawUTF8(), now[i].toRawUTF8());
        }
        CHECK (same == old.size(), "%d of %d controls identical to %s", same, old.size(), version);
        std::printf ("control list: %d controls, first %d identical to %s\n", now.size(), same, version);
    }
    for (int i = kNumControls; i < now.size(); ++i)
        CHECK (now[i].contains (" v2 "), "control added after 0.1.0 must be version 2: %s", now[i].toRawUTF8());
}

// ------------------------------------------------------------------------------------------------ 2. old projects

static void testOldProjects (const juce::File& goldenDir)
{
    for (const char* version : kVersions)
    {
        const auto xml = juce::parseXML (goldenDir.getChildFile (juce::String ("state-") + version + ".xml"));
        CHECK (xml != nullptr, "golden state %s readable", version);
        if (xml == nullptr) continue;
        CHECK (xml->getStringAttribute ("pluginVersion") == version, "golden state is from %s", version);
        juce::MemoryBlock mb;
        juce::AudioProcessor::copyXmlToBinary (*xml, mb);
        ParticleholdProcessor proc;
        proc.setStateInformation (mb.getData(), (int) mb.getSize());
        auto params = allParams (proc);
        int same = 0;
        for (int i = 0; i < kNumControls; ++i)
        {
            auto* p = params[(size_t) i];
            const float want = p->paramID == PhParams::trigger ? 0.0f : goldenValue (p, i, version);
            if (std::abs (p->getValue() - want) < 1.0e-4f) ++same;
            else std::printf ("  %s: %f, saved %f\n", p->paramID.toRawUTF8(), p->getValue(), want);
        }
        CHECK (same == kNumControls, "project saved by %s: %d of %d controls restored (the Trigger button released)", version, same, kNumControls);
        CHECK (P (proc, PhParams::trigger)->getValue() < 0.5f, "a project never opens with the Trigger button held");
        juce::MemoryBlock again;
        proc.getStateInformation (again);
        ParticleholdProcessor proc2;
        proc2.setStateInformation (again.getData(), (int) again.getSize());
        auto p2 = allParams (proc2);
        int same2 = 0;
        for (int i = 0; i < kNumControls; ++i) same2 += std::abs (p2[(size_t) i]->getValue() - params[(size_t) i]->getValue()) < 1.0e-6f ? 1 : 0;
        CHECK (same2 == kNumControls, "save and reopen: %d of %d", same2, kNumControls);
        // Playing after loading it: the note samples, and the clock ends low.
        prepare (proc);
        runEmpty (proc, 2);
        setReal (proc, PhParams::clock, (float) particlehold::clockNotes);
        const unsigned before = edges (proc);
        run (proc, one (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)), 512);
        runEmpty (proc, 20);
        run (proc, one (juce::MidiMessage::noteOff (1, 60)), 512);
        runEmpty (proc, 20);
        CHECK (edges (proc) == before + 1 && ! proc.getDisplay().gate.load() && ! proc.getDisplay().pulseOn.load(), "%s project: a note samples, and the clock ends low", version);
        std::printf ("old projects: %s restored (%d controls)\n", version, same);
    }
    ParticleholdProcessor proc;
    const char junk[] = "not a saved state";
    proc.setStateInformation (junk, (int) sizeof (junk));
    for (auto* p : allParams (proc)) CHECK (juce::approximatelyEqual (p->getValue(), p->getDefaultValue()), "junk state ignored (%s)", p->paramID.toRawUTF8());
    ParticleholdProcessor held;
    setReal (held, PhParams::trigger, 1.0f);
    juce::MemoryBlock mb;
    held.getStateInformation (mb);
    ParticleholdProcessor reopened;
    reopened.setStateInformation (mb.getData(), (int) mb.getSize());
    CHECK (P (reopened, PhParams::trigger)->getValue() < 0.5f, "Trigger button saved held reopens released");
}

// ------------------------------------------------------------------------------------------------ 3. flags

static void testFlags()
{
    ParticleholdProcessor proc;
    CHECK (proc.isMidiEffect() && proc.acceptsMidi() && proc.producesMidi(), "MIDI effect, MIDI in and out");
    CHECK (proc.getBusCount (true) == 0 && proc.getBusCount (false) == 0, "no audio buses (%d in, %d out)", proc.getBusCount (true), proc.getBusCount (false));
    CHECK (proc.getLatencySamples() == 0 && proc.getTailLengthSeconds() <= 0.0, "no latency, no tail");
    CHECK (proc.getName() == "particlehold", "name");
    CHECK ((int) allParams (proc).size() == kNumControls, "%d controls", (int) allParams (proc).size());
    auto* slew = P (proc, PhParams::slew);
    CHECK (juce::exactlyEqual (slew->convertFrom0to1 (0.0f), 0.0f) && slew->getCurrentValueAsText() == "Off", "Slew: far left is Off, the default");
    CHECK (std::abs (slew->convertFrom0to1 (0.001f) - 5.04f) < 0.05f && std::abs (slew->convertFrom0to1 (1.0f) - 10000.0f) < 0.5f, "Slew: 5 ms just right of Off, 10 s at the right");
    CHECK (std::abs (slew->convertFrom0to1 (0.5f) - 223.6f) < 0.5f, "Slew: centre 224 ms");
    CHECK (std::abs (slew->getValueForText ("1.2 s") - slew->convertTo0to1 (1200.0f)) < 1e-4f && juce::exactlyEqual (slew->getValueForText ("off"), 0.0f), "typing 1.2 s, and Off");
    auto txt = [&] (const char* id) { return P (proc, id)->getCurrentValueAsText(); };
    CHECK (txt (PhParams::mode) == "Sample and hold" && txt (PhParams::clock) == "Notes" && txt (PhParams::source) == "Noise"
           && txt (PhParams::range) == "Low" && txt (PhParams::sendAs) == "CC" && txt (PhParams::level) == "100%" && txt (PhParams::rate) == "50%",
           "defaults: Sample and hold, Notes, Noise, Low, Send as CC, Level 100%%, Rate 50%%");
    CHECK (juce::roundToInt (P (proc, PhParams::outChannel)->convertFrom0to1 (P (proc, PhParams::outChannel)->getValue())) == 13, "default MIDI out channel 13");
    CHECK (P (proc, PhParams::signalOn)->getValue() > 0.5f && P (proc, PhParams::noiseOn)->getValue() < 0.5f && P (proc, PhParams::clockOn)->getValue() > 0.5f
           && P (proc, PhParams::rectify)->getValue() < 0.5f, "defaults: Signal and Clock on, Noise off, rectifier off");
    CHECK (juce::roundToInt (P (proc, PhParams::sourceCc)->convertFrom0to1 (P (proc, PhParams::sourceCc)->getValue())) == 1, "default Source CC 1 (mod wheel)");
    const auto s = PhParams::read (proc.getValueTreeState());
    CHECK (std::abs (s.rateHz - 0.4472) < 1e-3, "Rate 50%%, Low: 0.45 Hz");
    setReal (proc, PhParams::range, 1.0f);
    CHECK (std::abs (PhParams::read (proc.getValueTreeState()).rateHz - 4.472) < 1e-2, "Rate 50%%, High: 4.5 Hz");
}

// ------------------------------------------------------------------------------------------------ 4. MIDI path

static void testMidiPath()
{
    ParticleholdProcessor proc;
    prepare (proc);
    auto first = run (proc, {}, 512);
    Receiver rx;
    rx.take (first);
    CHECK (first.size() == 2 && rx.value[13][0] == kMiddle, "first block: 0 V (the middle) sent once on channel 13 (%d messages)", (int) first.size());
    CHECK (run (proc, {}, 512).empty(), "nothing more while nothing happens");

    juce::MidiBuffer in;
    in.addEvent (juce::MidiMessage::controllerEvent (3, 1, 64), 10);
    in.addEvent (juce::MidiMessage::pitchWheel (3, 9000), 20);
    in.addEvent (juce::MidiMessage::channelPressureChange (3, 50), 30);
    in.addEvent (juce::MidiMessage::programChange (3, 7), 40);
    in.addEvent (juce::MidiMessage::aftertouchChange (3, 60, 33), 50);
    const juce::uint8 sysex[] = { 0x7d, 0x01, 0x02 };
    in.addEvent (juce::MidiMessage::createSysExMessage (sysex, 3), 60);
    in.addEvent (juce::MidiMessage::noteOn (3, 60, (juce::uint8) 90), 100);
    in.addEvent (juce::MidiMessage::controllerEvent (3, 64, 127), 100);
    auto out = run (proc, in, 512);
    std::vector<Out> theirs, ours;
    for (auto& o : out) (isOurCc (o.m) && o.m.getChannel() == 13 ? ours : theirs).push_back (o);
    CHECK (theirs.size() == 8, "all 8 incoming messages passed on (%d)", (int) theirs.size());
    if (theirs.size() == 8)
    {
        CHECK (theirs[0].pos == 10 && theirs[0].m.isController() && theirs[0].m.getControllerValue() == 64, "CC 1 unchanged");
        CHECK (theirs[1].pos == 20 && theirs[1].m.getPitchWheelValue() == 9000, "pitch bend unchanged");
        CHECK (theirs[2].pos == 30 && theirs[2].m.getChannelPressureValue() == 50, "channel pressure unchanged");
        CHECK (theirs[3].pos == 40 && theirs[3].m.getProgramChangeNumber() == 7, "program change unchanged");
        CHECK (theirs[4].pos == 50 && theirs[4].m.isAftertouch(), "poly aftertouch unchanged");
        CHECK (theirs[5].pos == 60 && theirs[5].m.isSysEx() && theirs[5].m.getSysExDataSize() == 3, "sysex unchanged");
        CHECK (theirs[6].pos == 100 && theirs[6].m.isNoteOn() && theirs[6].m.getNoteNumber() == 60 && theirs[6].m.getChannel() == 3
               && theirs[6].m.getVelocity() == 90, "note: same note, channel 3, velocity 90, sample 100");
        CHECK (theirs[7].pos == 100 && theirs[7].m.isSustainPedalOn(), "pedal after the note, same sample, in order");
    }
    bool sigAt = false, clockAt = false;
    for (auto& o : ours)
    {
        if (o.m.getControllerNumber() == 76 && o.pos == 100) sigAt = true;
        if (o.m.getControllerNumber() == 79 && o.m.getControllerValue() == 127 && o.pos == 100) clockAt = true;
    }
    CHECK (sigAt && clockAt, "the note's sample (100): a new Signal value (CC 75 / 76) and the Clock (CC 79) on");
    for (auto& o : ours) CHECK (o.m.getChannel() == 13, "particlehold's CC on channel 13");
    rx.take (out);
    CHECK (rx.value[13][0] >= 0 && std::abs (rx.value[13][0] - juce::roundToInt (proc.getDisplay().output.load() * 16383.0f)) <= 1, "the receiver has the held value the panel shows");
    // Release: velocity 0 counts as a note-off, and passes on unchanged; the Clock ends.
    ClockWatch w; w.take (out);
    auto rel = run (proc, one (juce::MidiMessage (0x92, 60, 0), 5), 512);
    w.take (rel);
    bool passed = false;
    for (auto& o : rel) if (o.m.isNoteOff() && o.m.getChannel() == 3 && o.pos == 5) passed = true;
    w.take (runEmpty (proc, 4));
    CHECK (passed && ! proc.getDisplay().gate.load() && ! w.anythingOn(), "velocity-0 note-on passed on, released the clock; the Clock ended");
    // 0- and 1-sample blocks.
    run (proc, one (juce::MidiMessage::noteOn (1, 62, (juce::uint8) 100)), 0);
    out = run (proc, one (juce::MidiMessage::noteOff (1, 62)), 1);
    CHECK (out.size() >= 1 && out[0].pos == 0, "0- and 1-sample blocks");
    runEmpty (proc, 10);
    CHECK (! proc.getDisplay().gate.load(), "and the note in the 0-sample block was released");
    // Another channel set: everything goes there.
    setReal (proc, PhParams::outChannel, 7);
    Receiver rx2;
    auto o2 = run (proc, one (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)), 512);
    rx2.take (o2);
    bool only7 = true;
    for (auto& o : o2) if (isOurCc (o.m)) only7 = only7 && (o.m.getChannel() == 7 || (o.m.getChannel() == 13 && o.pos == 0));
    CHECK (only7 && rx2.value[7][0] >= 0 && rx2.value[13][0] == kMiddle, "MIDI out channel 7: 0 V left on channel 13, then everything on channel 7");
    run (proc, one (juce::MidiMessage::noteOff (1, 60)), 512);
    runEmpty (proc, 10);

    // Send as Note and Both.
    for (int mode : { 1, 2 })
    {
        ParticleholdProcessor p2;
        prepare (p2);
        runEmpty (p2, 2);
        setReal (p2, PhParams::sendAs, (float) mode);
        ClockWatch cw;
        int ccs = 0;
        auto o = run (p2, one (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 90), 100), 512);
        cw.take (o);
        bool noteAt = false, playedPassed = false;
        for (auto& x : o)
        {
            if (x.m.isNoteOn() && x.m.getNoteNumber() == 48 && x.m.getVelocity() == 100 && x.pos == 100 && x.m.getChannel() == 13) noteAt = true;
            if (x.m.isNoteOn() && x.m.getNoteNumber() == 60 && x.m.getVelocity() == 90 && x.m.getChannel() == 1 && x.pos == 100) playedPassed = true;
            ccs += x.m.isController() && x.m.getControllerNumber() == 79 ? 1 : 0;
        }
        for (int i = 0; i < 20; ++i) for (auto& x : run (p2, {}, 512)) { cw.take (x.m); ccs += x.m.isController() && x.m.getControllerNumber() == 79 ? 1 : 0; }
        cw.take (run (p2, one (juce::MidiMessage::noteOff (1, 60)), 512));
        cw.take (runEmpty (p2, 4));
        CHECK (noteAt && playedPassed, "Send as %s: Clock note C2 (48) at the note's sample, velocity 100, channel 13; the played note passes unchanged", mode == 1 ? "Note" : "Both");
        CHECK ((mode == 1) == (ccs == 0), "Send as %s: %s", mode == 1 ? "Note" : "Both", mode == 1 ? "no Clock CC" : "Clock CC too");
        CHECK (! cw.anythingOn() && cw.doubled == 0 && cw.strayOff == 0, "Send as %s: the Clock note released, not doubled", mode == 1 ? "Note" : "Both");
    }
    // The internal clock runs without notes: Rate 100%, High = 40 Hz for 1 s.
    {
        ParticleholdProcessor p2;
        prepare (p2);
        setReal (p2, PhParams::clock, (float) particlehold::clockInternal);
        setReal (p2, PhParams::range, 1.0f);
        setReal (p2, PhParams::rate, 100.0f);
        int clockOn = 0;
        for (int i = 0; i < 94; ++i) for (auto& x : run (p2, {}, 512)) clockOn += x.m.isController() && x.m.getControllerNumber() == 79 && x.m.getControllerValue() == 127 ? 1 : 0;
        CHECK (std::abs ((int) edges (p2) - 40) <= 1 && clockOn == (int) edges (p2), "Internal, Rate 100%%, High: 40 samples and 40 Clock pulses in 1 s (%u, %d)", edges (p2), clockOn);
    }
    // Every source, read from the real MIDI.
    struct S { int source, cc; std::function<void (juce::MidiBuffer&)> send; int want; const char* name; };
    const std::vector<S> sources {
        { particlehold::sourcePitchBend, 1, [] (juce::MidiBuffer& b) { b.addEvent (juce::MidiMessage::pitchWheel (2, 12345), 10); }, 12345, "pitch bend 12345 comes out as 12345" },
        { particlehold::sourceCc, 20, [] (juce::MidiBuffer& b) { b.addEvent (juce::MidiMessage::controllerEvent (16, 20, 40), 10); b.addEvent (juce::MidiMessage::controllerEvent (16, 52, 99), 10); }, 40 * 128 + 99, "CC 20 + 52 (Ogdoad LFO 1) at full 14-bit" },
        { particlehold::sourceCc, 1, [] (juce::MidiBuffer& b) { b.addEvent (juce::MidiMessage::controllerEvent (1, 1, 127), 10); }, 16383, "the mod wheel at 127 is the very top" },
        { particlehold::sourceAftertouch, 1, [] (juce::MidiBuffer& b) { b.addEvent (juce::MidiMessage::channelPressureChange (1, 90), 10); }, 90 * 129, "channel pressure 90" },
        { particlehold::sourceAftertouch, 1, [] (juce::MidiBuffer& b) { b.addEvent (juce::MidiMessage::aftertouchChange (1, 60, 30), 10); }, 30 * 129, "polyphonic aftertouch 30" },
        { particlehold::sourceVelocity, 1, [] (juce::MidiBuffer&) {}, 77 * 129, "the note's own velocity 77" },
    };
    for (auto& s : sources)
    {
        ParticleholdProcessor p2;
        prepare (p2);
        runEmpty (p2, 1);
        setReal (p2, PhParams::source, (float) s.source);
        setReal (p2, PhParams::sourceCc, (float) s.cc);
        juce::MidiBuffer b;
        s.send (b);
        b.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 77), 100);
        Receiver r;
        r.take (run (p2, b, 512));
        CHECK (r.value[13][0] == s.want, "source: %s (got %d)", s.name, r.value[13][0]);
    }
}

// ------------------------------------------------------------------------------------------------ 5. automation

static void testAutomation()
{
    ParticleholdProcessor proc;
    prepare (proc);
    runEmpty (proc, 2);
    auto params = allParams (proc);
    int notes = 0, nonRest = 0;
    // No notes held, clock on Notes: moving any control (but the Trigger button and Clock, which can sample, and
    // the Noise switch, which starts the noise) sends at most 0 V (after a channel change or switching an output back
    // on), never anything else.
    for (auto* p : params)
    {
        if (p->paramID == PhParams::trigger || p->paramID == PhParams::clock || p->paramID == PhParams::noiseOn) continue;
        for (int i = 0; i <= 20; ++i)
        {
            p->setValueNotifyingHost ((float) i / 20.0f);
            for (auto& o : run (proc, {}, 256))
            {
                notes += o.m.isNoteOnOrOff() ? 1 : 0;
                if (o.m.isController() && o.m.getControllerNumber() == 79 && o.m.getControllerValue() != 0) ++nonRest;
                if (o.m.isController() && (o.m.getControllerNumber() == 75 || o.m.getControllerNumber() == 77) && o.m.getControllerValue() != 64 && o.m.getControllerValue() != 0) ++nonRest;
            }
        }
        p->setValueNotifyingHost (p->getDefaultValue());
        runEmpty (proc, 2);
    }
    CHECK (notes == 0 && nonRest == 0, "moving controls with no note held: only 0 V (%d notes, %d others)", notes, nonRest);
    CHECK (edges (proc) == 0, "and no sample moment (%u)", edges (proc));

    // A note held: automate everything hard. Never more than one value per output per block; the Clock never more
    // than two on and two off per block, never a doubled note.
    ClockWatch watch;
    run (proc, one (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)), 512);
    std::mt19937 rng (42);
    int bad = 0, burst = 0;
    for (int k = 0; k < 3000; ++k)
    {
        auto* p = params[rng() % params.size()];
        if (p->paramID == PhParams::trigger) continue;
        p->setValueNotifyingHost ((float) (rng() % 1001) / 1000.0f);
        const auto out = run (proc, {}, 64 + (int) (rng() % 512));
        watch.take (out);
        std::map<std::pair<int, int>, int> perFine;
        int perClock = 0;
        for (auto& o : out)
        {
            if (o.m.isController() && (o.m.getControllerNumber() == 76 || o.m.getControllerNumber() == 78)) ++perFine[{ o.m.getChannel(), o.m.getControllerNumber() }];
            if ((o.m.isController() && o.m.getControllerNumber() == 79) || isClockNote (o.m)) ++perClock;
            notes += o.m.isNoteOnOrOff() && ! isClockNote (o.m) ? 1 : 0;
        }
        for (auto& [key, count] : perFine) bad += count > 1 ? 1 : 0;
        burst += perClock > 4 ? 1 : 0;
    }
    CHECK (notes == 0, "automation while held sends no other notes (%d)", notes);
    CHECK (bad == 0, "automation while held: never more than one value per output per block (%d blocks over)", bad);
    CHECK (burst == 0 && watch.doubled == 0 && watch.strayOff == 0, "automation while held: no Clock bursts (%d), no doubled or stray Clock notes (%d, %d)", burst, watch.doubled, watch.strayOff);
    for (auto* p : params) p->setValueNotifyingHost (p->getDefaultValue());
    watch.take (run (proc, one (juce::MidiMessage::noteOff (1, 60)), 512));
    for (int i = 0; i < 10; ++i) watch.take (run (proc, {}, 512));
    CHECK (! proc.getDisplay().gate.load() && ! watch.anythingOn(), "after automation and release: the clock low, no Clock left on, on any channel");

    // A key held, the Clock sent as both: Send as, the channel and the switches changed over and over. These only ever
    // end the Clock gate; none of them starts one.
    {
        ParticleholdProcessor p2;
        prepare (p2);
        setReal (p2, PhParams::sendAs, 2.0f);
        ClockWatch w;
        w.take (run (p2, one (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)), 512));
        for (int i = 0; i < 4; ++i) w.take (run (p2, {}, 512));
        const int startsBefore = w.starts;
        CHECK (w.noteOn[13] && w.ccVal[13] == 127 && startsBefore == 2, "held: the Clock on as a note and as CC");
        std::mt19937 r2 (7);
        const char* changeable[] = { PhParams::sendAs, PhParams::outChannel, PhParams::clockOn, PhParams::signalOn, PhParams::noiseOn, PhParams::mode };
        for (int k = 0; k < 600; ++k)
        {
            P (p2, changeable[r2() % 6])->setValueNotifyingHost ((float) (r2() % 1001) / 1000.0f);
            w.take (run (p2, {}, 32 + (int) (r2() % 600)));
        }
        CHECK (p2.getDisplay().gate.load() && edges (p2) == 1, "still held, still one sample moment (%u)", edges (p2));
        CHECK (w.starts == startsBefore && w.doubled == 0 && w.strayOff == 0, "changing Send as, channel, switches and mode while held never starts the Clock (%d new)", w.starts - startsBefore);
        for (auto* p : allParams (p2)) p->setValueNotifyingHost (p->getDefaultValue());
        w.take (run (p2, one (juce::MidiMessage::noteOff (1, 60)), 512));
        for (int i = 0; i < 10; ++i) w.take (run (p2, {}, 512));
        CHECK (! w.anythingOn(), "released: nothing left on");
    }
}

// ------------------------------------------------------------------------------------------------ 6. stops

static void testStops()
{
    struct Case { const char* name; std::function<void (ParticleholdProcessor&, FakePlayHead&, double&)> stop; bool bypass; };
    const std::vector<Case> cases {
        { "transport stop", [] (ParticleholdProcessor&, FakePlayHead& ph, double&) { ph.playing = false; }, false },
        { "reset", [] (ParticleholdProcessor& p, FakePlayHead&, double&) { p.reset(); }, false },
        { "releaseResources", [] (ParticleholdProcessor& p, FakePlayHead&, double&) { p.releaseResources(); }, false },
        { "prepareToPlay", [] (ParticleholdProcessor& p, FakePlayHead&, double&) { p.prepareToPlay (44100.0, 256); }, false },
        { "All Notes Off", [] (ParticleholdProcessor&, FakePlayHead&, double&) {}, false },
        { "bypass", [] (ParticleholdProcessor&, FakePlayHead&, double&) {}, true },
        { "a 2 s pause in Logic's calls", [] (ParticleholdProcessor&, FakePlayHead&, double& t) { t += 2000.0; }, false },
    };
    for (int mode : { 0, 2 })   // the Clock sent as CC, then as CC and notes
    for (const auto& c : cases)
    {
        ParticleholdProcessor proc;
        FakePlayHead ph;
        ph.playing = true;
        proc.setPlayHead (&ph);
        double t = 0.0;
        proc.clock = [&t] { return t; };
        prepare (proc);
        setReal (proc, PhParams::sendAs, (float) mode);
        setReal (proc, PhParams::mode, (float) particlehold::modeHoldTrack);   // tracking the noise while held
        Receiver rx;
        ClockWatch w;
        auto step = [&] (juce::MidiBuffer m, bool bypassed = false) { t += 10.0; auto o = run (proc, m, 512, bypassed); rx.take (o); w.take (o); return o; };
        step ({});
        step (one (juce::MidiMessage::noteOn (2, 64, (juce::uint8) 100)));
        for (int i = 0; i < 10; ++i) step ({});
        CHECK (proc.getDisplay().tracking.load() && rx.value[13][0] != kMiddle, "%s: tracking before", c.name);
        CHECK (w.ccVal[13] == 127 && w.noteOn[13] == (mode == 2), "%s: the Clock on before (%s)", c.name, mode == 2 ? "CC and note" : "CC");
        c.stop (proc, ph, t);
        if (juce::String (c.name) == "All Notes Off")
        {
            step (one (juce::MidiMessage::allNotesOff (2)));
            CHECK (! proc.getDisplay().tracking.load() && ! proc.getDisplay().gate.load(), "All Notes Off: the clock falls, the value is held");
            for (int i = 0; i < 10; ++i) step ({});
            CHECK (! w.anythingOn(), "All Notes Off: the Clock ended");
        }
        else
        {
            const auto o = step ({}, c.bypass);
            bool restFirst = false;
            for (auto& m : o) if (m.pos == 0 && m.m.isController() && m.m.getControllerNumber() == 76 && m.m.getControllerValue() == 0) restFirst = true;
            CHECK (! proc.getDisplay().gate.load() && ! proc.getDisplay().tracking.load(), "%s: at rest at once", c.name);
            CHECK (restFirst && rx.value[13][0] == kMiddle, "%s: 0 V sent at the first sample", c.name);
            int ends = 0;
            for (auto& m : o)
                if (m.pos == 0 && ((m.m.isController() && m.m.getControllerNumber() == 79 && m.m.getControllerValue() == 0) || (m.m.isNoteOff() && m.m.getNoteNumber() == 48)))
                    ++ends;
            CHECK (ends == (mode == 2 ? 2 : 1) && ! w.anythingOn(), "%s: the Clock ended at the first sample (%s)", c.name, mode == 2 ? "CC 0 and note-off" : "CC 0");
            if (c.bypass)
            {
                int ours = 0;
                for (int i = 0; i < 20; ++i) for (auto& m : step (one (juce::MidiMessage::noteOn (1, 70, (juce::uint8) 100)), true)) ours += isOurCc (m.m) || isClockNote (m.m) ? 1 : 0;
                CHECK (ours == 0, "bypassed: notes pass, nothing is sent (%d)", ours);
                step (one (juce::MidiMessage::noteOff (1, 70)), true);
            }
            // The key's later release passes on and changes nothing; nothing more is sent.
            ph.playing = false;
            const auto o2 = step (one (juce::MidiMessage::noteOff (2, 64)));
            int ours = 0; bool offPassed = false;
            for (auto& m : o2) { ours += isOurCc (m.m) ? 1 : 0; offPassed = offPassed || (m.m.isNoteOff() && m.m.getNoteNumber() == 64); }
            for (int i = 0; i < 20; ++i) for (auto& m : step ({})) ours += isOurCc (m.m) ? 1 : 0;
            CHECK (offPassed && ours == 0, "%s: the key's release passes on, nothing else happens (%d)", c.name, ours);
        }
        // And the next note plays normally.
        const unsigned before = edges (proc);
        step (one (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)));
        CHECK (edges (proc) == before + 1 && proc.getDisplay().tracking.load(), "%s: the next note plays", c.name);
        step (one (juce::MidiMessage::noteOff (1, 60)));
        for (int i = 0; i < 10; ++i) step ({});
        CHECK (! proc.getDisplay().gate.load() && ! w.anythingOn() && w.doubled == 0 && w.strayOff == 0, "%s: and nothing left on (%s)", c.name, mode == 2 ? "Both" : "CC");
        proc.setPlayHead (nullptr);
    }
    // The internal clock: a transport stop sends 0 V, the next tick comes half a tick later.
    {
        ParticleholdProcessor proc;
        FakePlayHead ph;
        ph.playing = true;
        proc.setPlayHead (&ph);
        prepare (proc);
        setReal (proc, PhParams::clock, (float) particlehold::clockInternal);
        setReal (proc, PhParams::range, 1.0f);
        setReal (proc, PhParams::rate, 50.0f);   // 4.47 Hz
        runEmpty (proc, 40);
        ph.playing = false;
        const auto o = run (proc, {}, 512);
        bool zeroFirst = false;
        for (auto& m : o) if (m.m.isController() && m.m.getControllerNumber() == 76 && m.pos == 0 && m.m.getControllerValue() == 0) zeroFirst = true;
        const unsigned at = edges (proc);
        runEmpty (proc, 9);    // 0.096 s: before half a tick (0.112 s)
        const unsigned mid = edges (proc);
        runEmpty (proc, 3);
        CHECK (zeroFirst && mid == at && edges (proc) == at + 1, "internal clock, transport stop: 0 V sent, the next tick half a tick later");
        proc.setPlayHead (nullptr);
    }
}

// ------------------------------------------------------------------------------------------------ 7. fuzz

static void fuzz (unsigned seed, bool withBypass)
{
    ParticleholdProcessor proc;
    FakePlayHead ph;
    proc.setPlayHead (&ph);
    double t = 0.0;
    proc.clock = [&t] { return t; };
    prepare (proc);
    std::mt19937 rng (seed);
    auto params = allParams (proc);
    Receiver rx;
    ClockWatch w;
    bool down[17][128] {};
    long long events = 0;
    for (int block = 0; block < 18000; ++block)
    {
        t += rng() % 400 == 0 ? 1500.0 : 10.0;
        const int n = rng() % 40 == 0 ? (int) (rng() % 2) : 1 + (int) (rng() % 1024);
        juce::MidiBuffer in;
        const int count = (int) (rng() % 6);
        for (int k = 0; k < count; ++k)
        {
            const int at = n > 0 ? (int) (rng() % (unsigned) n) : 0, ch = 1 + (int) (rng() % 3), note = 58 + (int) (rng() % 6);
            const unsigned kind = rng() % 26;
            if (kind == 0)      { in.addEvent (juce::MidiMessage::allNotesOff (ch), at); for (auto& d : down[ch]) d = false; }
            else if (kind < 3)  in.addEvent (juce::MidiMessage::controllerEvent (ch, (int) (rng() % 64), (int) (rng() % 128)), at);
            else if (kind < 4)  in.addEvent (juce::MidiMessage::pitchWheel (ch, (int) (rng() % 16384)), at);
            else if (kind < 13) { in.addEvent (juce::MidiMessage::noteOn (ch, note, (juce::uint8) (1 + rng() % 127)), at); down[ch][note] = true; }
            else if (kind < 15) { in.addEvent (juce::MidiMessage (0x90 + ch - 1, note, 0), at); down[ch][note] = false; }
            else                { in.addEvent (juce::MidiMessage::noteOff (ch, note), at); down[ch][note] = false; }
            ++events;
        }
        if (rng() % 30 == 0)
        {
            auto* p = params[rng() % params.size()];
            p->setValueNotifyingHost ((float) (rng() % 1001) / 1000.0f);
        }
        if (rng() % 200 == 0) ph.playing = ! ph.playing;
        if (rng() % 500 == 0) proc.reset();
        const bool bypassed = withBypass && (block / 300) % 4 == 3;
        const auto o = run (proc, in, n, bypassed);
        rx.take (o);
        w.take (o);
    }
    // Release everything (and the Trigger button), the clock on Notes, then wait.
    juce::MidiBuffer offs;
    for (int ch = 1; ch <= 16; ++ch) for (int nt = 0; nt < 128; ++nt) if (down[ch][nt]) offs.addEvent (juce::MidiMessage::noteOff (ch, nt), 0);
    setReal (proc, PhParams::trigger, 0.0f);
    setReal (proc, PhParams::clock, (float) particlehold::clockNotes);
    t += 10.0;
    { const auto o = run (proc, offs, 512); rx.take (o); w.take (o); }
    for (int i = 0; i < 20; ++i) { t += 10.0; const auto o = run (proc, {}, 512); rx.take (o); w.take (o); }
    CHECK (! proc.getDisplay().gate.load() && ! proc.getDisplay().pulseOn.load(), "fuzz %u: the clock low after every key is released", seed);
    CHECK (! w.anythingOn() && w.doubled == 0 && w.strayOff == 0, "fuzz %u: no Clock CC or note left on, none doubled (%d starts)", seed, w.starts);
    // A rest: every channel's receiver at 0 V (the middle, or the bottom where the rectifier was on).
    proc.reset();
    setReal (proc, PhParams::noiseOn, 0.0f);
    { t += 10.0; const auto o = run (proc, {}, 512); rx.take (o); w.take (o); }
    bool allZero = true;
    for (int ch = 1; ch <= 16; ++ch)
        for (int k = 0; k < 2; ++k)
            allZero = allZero && (rx.value[ch][k] < 0 || rx.value[ch][k] == kMiddle || (k == 0 && rx.value[ch][k] == 0));
    CHECK (allZero, "fuzz %u: after a rest every channel's receiver at 0 V", seed);
    std::printf ("stuck-gate and stuck-note fuzz%s: %lld random events in 18,000 blocks, %d Clock starts, nothing stuck\n", withBypass ? " with bypass" : "", events, w.starts);
    proc.setPlayHead (nullptr);
}

// ------------------------------------------------------------------------------------------------ 8. panel

static void testPanel()
{
    ParticleholdProcessor proc;
    prepare (proc);
    runEmpty (proc, 2);
    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditorAndMakeActive());
    auto* editor = dynamic_cast<ParticleholdEditor*> (ed.get());
    CHECK (editor != nullptr, "panel opens");
    if (editor == nullptr) return;
    CHECK (ed->getWidth() == ParticleholdEditor::kWidth && ed->getHeight() == ParticleholdEditor::kHeight, "design size %dx%d", ed->getWidth(), ed->getHeight());
    const auto image = ed->createComponentSnapshot (ed->getLocalBounds());
    CHECK (image.isValid() && image.getWidth() == ParticleholdEditor::kWidth, "panel draws");

    // Every control inside the panel, live, moves only its own parameter.
    auto params = allParams (proc);
    CHECK ((int) editor->panel.controls().size() == kNumControls, "every control is on the panel (%d)", (int) editor->panel.controls().size());
    for (auto& c : editor->panel.controls())
    {
        CHECK (juce::Rectangle<int> (0, 0, ParticleholdPanel::kWidth, ParticleholdPanel::kHeight).contains (c.control->getBounds()), "%s inside the panel", c.paramId.toRawUTF8());
        CHECK (c.control->isEnabled() && c.control->isVisible(), "%s live", c.paramId.toRawUTF8());
        std::vector<float> before;
        for (auto* p : params) before.push_back (p->getValue());
        if (auto* s = dynamic_cast<juce::Slider*> (c.control))            s->setValue (s->getMinimum() + (s->getMaximum() - s->getMinimum()) * 0.71, juce::sendNotificationSync);
        else if (auto* b = dynamic_cast<juce::ToggleButton*> (c.control)) b->setToggleState (! b->getToggleState(), juce::sendNotificationSync);
        else if (auto* cb = dynamic_cast<juce::ComboBox*> (c.control))     cb->setSelectedItemIndex (cb->getSelectedItemIndex() == 0 ? 2 : 0, juce::sendNotificationSync);
        else if (auto* sg = dynamic_cast<Segmented*> (c.control))          sg->choose (sg->selected() == 0 ? 1 : 0);
        else if (auto* tb = dynamic_cast<TriggerButton*> (c.control))      tb->press (true);
        int changed = 0; bool own = false;
        for (size_t i = 0; i < params.size(); ++i)
            if (std::abs (params[i]->getValue() - before[i]) > 1e-6f) { ++changed; own = own || params[i]->paramID == c.paramId; }
        CHECK (changed == 1 && own, "%s moves only its own parameter (%d changed)", c.paramId.toRawUTF8(), changed);
        if (auto* tb = dynamic_cast<TriggerButton*> (c.control)) tb->press (false);
        for (size_t i = 0; i < params.size(); ++i) params[i]->setValueNotifyingHost (params[i]->getDefaultValue());
    }
    runEmpty (proc, 4);

    // The texts under the knobs.
    auto& panel = editor->panel;
    CHECK (panel.labelFor (PhParams::level) == "level 100%" && panel.labelFor (PhParams::slew) == "slew Off" && panel.labelFor (PhParams::rate) == "rate 0.45 Hz",
           "texts: %s, %s, %s", panel.labelFor (PhParams::level).toRawUTF8(), panel.labelFor (PhParams::slew).toRawUTF8(), panel.labelFor (PhParams::rate).toRawUTF8());
    setReal (proc, PhParams::range, 1.0f);
    setReal (proc, PhParams::slew, 1200.0f);
    CHECK (panel.labelFor (PhParams::rate) == "rate 4.5 Hz" && panel.labelFor (PhParams::slew) == "slew 1.20 s", "texts follow Range and Slew: %s, %s",
           panel.labelFor (PhParams::rate).toRawUTF8(), panel.labelFor (PhParams::slew).toRawUTF8());
    setReal (proc, PhParams::range, 0.0f);
    setReal (proc, PhParams::slew, 0.0f);

    // The Trigger button samples; the clock light follows; in hold and track the track light while held.
    auto& now = panel.getNow();
    const unsigned e0 = edges (proc);
    panel.getTriggerButton().press (true);
    runEmpty (proc, 2);
    now.refresh();
    CHECK (edges (proc) == e0 + 1 && proc.getDisplay().gate.load() && now.clockLit(), "Trigger button: a sample moment, the clock light lit");
    panel.getTriggerButton().press (false);
    runEmpty (proc, 6);
    for (int i = 0; i < 5; ++i) now.refresh();
    CHECK (! now.clockLit() && now.status() == "holding", "Trigger button let go: the clock light out, holding");
    panel.getModeSwitch().choose (particlehold::modeHoldTrack);
    panel.getTriggerButton().press (true);
    runEmpty (proc, 2);
    now.refresh();
    CHECK (now.trackLit() && now.status() == "tracking", "hold and track, Trigger held: the track light lit, tracking");
    panel.getTriggerButton().press (false);
    runEmpty (proc, 2);
    now.refresh();
    CHECK (! now.trackLit(), "let go: the track light out");
    panel.getModeSwitch().choose (particlehold::modeSampleHold);

    // The output lights and the scope follow the engine.
    auto& lights = panel.getOutputLights();
    run (proc, one (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)), 512);
    runEmpty (proc, 3);
    lights.refresh();
    CHECK (lights.level (0) > 0.0f && juce::exactlyEqual (lights.level (1), 0.0f) && lights.level (2) > 0.0f, "a note: Signal and Clock lights lit, Noise (off) dark");
    run (proc, one (juce::MidiMessage::noteOff (1, 60)), 512);
    runEmpty (proc, 10);
    for (int i = 0; i < 5; ++i) lights.refresh();
    CHECK (juce::exactlyEqual (lights.level (2), 0.0f), "released: the Clock light out");
    setReal (proc, PhParams::noiseOn, 1.0f);
    runEmpty (proc, 3);
    lights.refresh();
    CHECK (lights.level (1) > 0.0f, "Noise on: its light lit");
    setReal (proc, PhParams::noiseOn, 0.0f);
    auto& scope = panel.getScope();
    scope.refresh();
    const auto pt = scope.newest();
    CHECK (pt.x > 0.0f && std::abs (pt.y - scope.yOf (proc.getDisplay().output.load())) < 0.5f, "the scope's newest point is the present output (%.1f)", pt.y);
    CHECK (proc.getDisplay().scopeCount.load() > 30, "the scope fills (%llu points)", (unsigned long long) proc.getDisplay().scopeCount.load());

    // Resizing: 80% to 200%, proportions locked, size saved with the project.
    ed->setSize (ParticleholdEditor::kWidth * 3 / 2, ParticleholdEditor::kHeight * 3 / 2);
    CHECK (ed->getWidth() == ParticleholdEditor::kWidth * 3 / 2, "panel resizes to 150%%");
    auto* cons = ed->getConstrainer();
    CHECK (cons != nullptr && cons->getMinimumWidth() == 624 && cons->getMaximumWidth() == 1560 && std::abs (cons->getFixedAspectRatio() - 780.0 / 470.0) < 1e-9,
           "size limits 624..1560 wide, proportions locked");
    const int savedWidth = (int) proc.getValueTreeState().state.getProperty (ParticleholdEditor::widthProperty, -1);
    CHECK (savedWidth == ParticleholdEditor::kWidth * 3 / 2, "window size saved with the project (%d)", savedWidth);
    proc.editorBeingDeleted (ed.get());
    ed.reset();
    juce::MemoryBlock mb;
    proc.getStateInformation (mb);
    ParticleholdProcessor proc2;
    proc2.setStateInformation (mb.getData(), (int) mb.getSize());
    std::unique_ptr<juce::AudioProcessorEditor> ed2 (proc2.createEditorAndMakeActive());
    CHECK (ed2->getWidth() == ParticleholdEditor::kWidth * 3 / 2, "reopened at the saved size (%d)", ed2->getWidth());
    proc2.editorBeingDeleted (ed2.get());
}

static void writeSnapshots (const juce::File& dir)
{
    dir.createDirectory();
    auto shoot = [&] (const char* name, int width, std::function<void (ParticleholdProcessor&)> setup) {
        ParticleholdProcessor proc;
        prepare (proc);
        runEmpty (proc, 2);
        setup (proc);
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditorAndMakeActive());
        ed->setSize (width, ParticleholdEditor::heightForWidth (width));
        auto& panel = dynamic_cast<ParticleholdEditor*> (ed.get())->panel;
        panel.getScope().refresh(); panel.getNow().refresh(); panel.getOutputLights().refresh();
        const auto image = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
        juce::PNGImageFormat png;
        juce::FileOutputStream os (dir.getChildFile (name));
        os.setPosition (0); os.truncate();
        png.writeImageToStream (image, os);
        proc.editorBeingDeleted (ed.get());
        std::printf ("wrote %s\n", dir.getChildFile (name).getFullPathName().toRawUTF8());
    };
    // Notes every 300 ms for 4 s (the scope full), the last one held.
    auto play = [] (ParticleholdProcessor& p, bool releaseLast) {
        for (int k = 0; k < 13; ++k)
        {
            run (p, one (juce::MidiMessage::noteOn (1, 60 + k % 5, (juce::uint8) 100)), 512);
            runEmpty (p, 13);
            if (k < 12 || releaseLast) run (p, one (juce::MidiMessage::noteOff (1, 60 + k % 5)), 512);
            runEmpty (p, 14);
        }
    };
    shoot ("window-1-default.png", 780, [] (ParticleholdProcessor&) {});
    shoot ("window-2-notes-sample-and-hold.png", 780, [&] (ParticleholdProcessor& p) { play (p, false); });
    shoot ("window-3-slew-300ms.png", 780, [&] (ParticleholdProcessor& p) { setReal (p, PhParams::slew, 300.0f); play (p, true); });
    shoot ("window-4-hold-and-track-held.png", 780, [&] (ParticleholdProcessor& p) {
        setReal (p, PhParams::mode, (float) particlehold::modeHoldTrack);
        setReal (p, PhParams::slew, 40.0f);
        play (p, false);
    });
    shoot ("window-5-internal-rectified-noise-out.png", 780, [&] (ParticleholdProcessor& p) {
        setReal (p, PhParams::clock, (float) particlehold::clockInternal);
        setReal (p, PhParams::range, 1.0f);
        setReal (p, PhParams::rate, 40.0f);
        setReal (p, PhParams::rectify, 1.0f);
        setReal (p, PhParams::level, 70.0f);
        setReal (p, PhParams::noiseOn, 1.0f);
        runEmpty (p, 400);
    });
    shoot ("window-6-small-80.png", 624, [&] (ParticleholdProcessor& p) { play (p, false); });
    shoot ("window-7-send-as-note-source-cc.png", 780, [&] (ParticleholdProcessor& p) {
        setReal (p, PhParams::sendAs, 1.0f);
        setReal (p, PhParams::source, (float) particlehold::sourceCc);
        setReal (p, PhParams::sourceCc, 20.0f);
        play (p, false);
    });
    shoot ("window-8-send-as-both-ch3.png", 780, [&] (ParticleholdProcessor& p) { setReal (p, PhParams::sendAs, 2.0f); setReal (p, PhParams::outChannel, 3.0f); play (p, false); });
    shoot ("window-9-large-200.png", 1560, [&] (ParticleholdProcessor& p) { play (p, false); });
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    bool writeGolden = false;
    juce::File goldenDir (PARTICLEHOLD_GOLDEN_DIR);
    for (int i = 1; i < argc; ++i)
    {
        if (juce::String (argv[i]) == "--write-golden") writeGolden = true;
        if (juce::String (argv[i]) == "--snapshot" && i + 1 < argc) { writeSnapshots (juce::File (argv[i + 1])); return 0; }
    }
    testControlList (goldenDir, writeGolden);
    if (writeGolden) return 0;
    testOldProjects (goldenDir);
    testFlags();
    testMidiPath();
    testAutomation();
    testStops();
    fuzz (777, false);
    fuzz (4242, true);
    testPanel();
    std::printf ("plugin checks: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
