// What Logic sees: a MIDI effect (AU type aumi) with no audio. Every incoming MIDI message passes through unchanged,
// at its own sample, with no delay. The notes are also read (the clock, when Clock is Notes) and so are the
// controllers, aftertouch, pitch bend and velocities (the Source). Signal and Noise go out as 14-bit CC on the chosen
// channel (13 by default), the Clock as CC and/or a note, added to the MIDI stream at the exact samples.
// Adapted from Bryn 0.1.0.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <particlehold/Engine.h>
#include "Parameters.h"

#include <atomic>
#include <functional>
#include <vector>

// What the panel shows (written by the audio thread after every block, read by the panel).
struct ParticleholdDisplay
{
    std::atomic<float> output { 0.5f }, input { 0.5f };            // 0 .. 1
    std::atomic<bool> gate { false }, tracking { false }, rectified { false }, gliding { false };
    std::atomic<unsigned> edges { 0 };                               // sample moments so far
    std::atomic<int> sent[particlehold::kNumSmoothOutputs] { { -1 }, { -1 } };   // the last 14-bit value each output sent
    std::atomic<bool> pulseOn { false };                             // the Clock output is on now
    std::atomic<unsigned> pulseStarts { 0 };                         // how many times it has come on
    std::atomic<unsigned> blocks { 0 };

    // The scope: a point every 10 ms (input, output, a sample moment in it), the last kScope kept.
    static constexpr int kScope = particlehold::Engine::kScopeSize;
    std::atomic<float> scopeIn[kScope] {}, scopeOut[kScope] {};
    std::atomic<bool> scopeEdge[kScope] {};
    std::atomic<unsigned long long> scopeCount { 0 };
};

class ParticleholdProcessor final : public juce::AudioProcessor
{
public:
    ParticleholdProcessor();
    ~ParticleholdProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "particlehold"; }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return true; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getValueTreeState() { return apvts; }
    const ParticleholdDisplay& getDisplay() const { return display; }

    // If Logic stops calling the plugin for this long and then calls again, everything goes to rest first (a key
    // released while the plugin was not being run can never leave the gate held). Replaceable for the checks.
    static constexpr double kGapMs = 1000.0;
    std::function<double()> clock = [] { return juce::Time::getMillisecondCounterHiRes(); };

private:
    struct Sink final : particlehold::MidiSink
    {
        juce::MidiBuffer* out = nullptr;
        int last = 0;
        void cc (int offset, int channel, int number, int value) override
        {
            out->addEvent (juce::MidiMessage::controllerEvent (channel, number, value), juce::jlimit (0, last, offset));
        }
        void note (int offset, int channel, int number, int velocity) override
        {
            const int at = juce::jlimit (0, last, offset);
            if (velocity > 0) out->addEvent (juce::MidiMessage::noteOn (channel, number, (juce::uint8) velocity), at);
            else              out->addEvent (juce::MidiMessage::noteOff (channel, number), at);
        }
    };

    void process (juce::MidiBuffer& midi, int numSamples);
    void processBypassed (juce::MidiBuffer& midi, int numSamples);
    void publish();

    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* manualParam = nullptr;
    particlehold::Engine engine;            // used by the audio thread only
    std::vector<particlehold::InputEvent> events;
    juce::MidiBuffer out;
    Sink sink;
    std::atomic<bool> pendingRest { false };
    bool wasPlaying = false, wasBypassed = false;
    double lastCall = -1.0;
    ParticleholdDisplay display;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParticleholdProcessor)
};
