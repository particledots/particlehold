#include "PluginProcessor.h"
#include "PluginEditor.h"

ParticleholdProcessor::ParticleholdProcessor()
    : AudioProcessor (BusesProperties()),   // a MIDI effect: no audio buses
      apvts (*this, nullptr, "particlehold", PhParams::createLayout())
{
    manualParam = apvts.getRawParameterValue (PhParams::trigger);
    events.reserve (4096);
    out.ensureSize (65536);
    // Each copy of the plugin gets its own noise.
    engine.setSeed ((std::uint64_t) juce::Random::getSystemRandom().nextInt64() ^ (std::uint64_t) juce::Time::getHighResolutionTicks());
    engine.prepare (48000.0);
}

bool ParticleholdProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet().isDisabled() && layouts.getMainInputChannelSet().isDisabled();
}

// Logic may call these when it stops, resets or rebuilds the plugin. A MIDI effect cannot send MIDI from here, so the
// next block starts by sending everything to rest (0 V sent, the Clock gate ended).
void ParticleholdProcessor::prepareToPlay (double sampleRate, int)
{
    engine.prepare (sampleRate);      // at rest, everything sent afresh
    out.ensureSize (65536);
    pendingRest = true;
}
void ParticleholdProcessor::releaseResources() { pendingRest = true; }
void ParticleholdProcessor::reset()            { pendingRest = true; }

void ParticleholdProcessor::processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer& m)          { process (m, b.getNumSamples()); }
void ParticleholdProcessor::processBlock (juce::AudioBuffer<double>& b, juce::MidiBuffer& m)         { process (m, b.getNumSamples()); }
void ParticleholdProcessor::processBlockBypassed (juce::AudioBuffer<float>& b, juce::MidiBuffer& m)  { processBypassed (m, b.getNumSamples()); }
void ParticleholdProcessor::processBlockBypassed (juce::AudioBuffer<double>& b, juce::MidiBuffer& m) { processBypassed (m, b.getNumSamples()); }

// The whole MIDI path. Every incoming event passes on unchanged at its own sample; notes, controllers, pitch bend and
// aftertouch are also read (velocity 0 counts as a release; All Notes Off / All Sound Off release that channel's keys).
// The engine adds its MIDI.
void ParticleholdProcessor::process (juce::MidiBuffer& midi, int numSamples)
{
    const double now = clock();
    if (lastCall >= 0.0 && now - lastCall > kGapMs) pendingRest = true;
    lastCall = now;
    wasBypassed = false;

    if (pendingRest.exchange (false)) engine.panic();
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            const bool playing = pos->getIsPlaying();
            if (wasPlaying && ! playing) engine.panic();   // transport stop: rest at the first stopped block
            wasPlaying = playing;
        }

    engine.setSettings (PhParams::read (apvts));

    using IE = particlehold::InputEvent;
    out.clear();
    events.clear();
    const int last = juce::jmax (0, numSamples - 1);
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        const int at = juce::jlimit (0, last, meta.samplePosition);
        out.addEvent (m, at);
        if (events.size() >= 4000) continue;
        const auto ch = (std::uint8_t) juce::jlimit (0, 15, m.getChannel() - 1);
        if (m.isNoteOn())
            events.push_back ({ at, IE::noteOn, ch, (std::uint8_t) m.getNoteNumber(), (int) m.getVelocity() });
        else if (m.isNoteOff())   // includes velocity 0
            events.push_back ({ at, IE::noteOff, ch, (std::uint8_t) m.getNoteNumber(), 0 });
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            events.push_back ({ at, IE::allNotesOff, ch, 0, 0 });
        else if (m.isController())
            events.push_back ({ at, IE::controller, ch, (std::uint8_t) m.getControllerNumber(), m.getControllerValue() });
        else if (m.isPitchWheel())
            events.push_back ({ at, IE::pitchBend, ch, 0, m.getPitchWheelValue() });
        else if (m.isChannelPressure())
            events.push_back ({ at, IE::pressure, ch, 0, m.getChannelPressureValue() });
        else if (m.isAftertouch())   // polyphonic aftertouch counts as aftertouch too (the latest of either)
            events.push_back ({ at, IE::pressure, ch, 0, m.getAfterTouchValue() });
    }

    sink.out = &out;
    sink.last = last;
    engine.render (numSamples, events.data(), (int) events.size(), manualParam->load() >= 0.5f, sink);
    midi.swapWith (out);
    publish();
}

// Bypassed: incoming MIDI passes untouched and nothing is read. Everything goes to rest once (0 V sent, the Clock gate
// ended) and stays there until the bypass is lifted.
void ParticleholdProcessor::processBypassed (juce::MidiBuffer& midi, int numSamples)
{
    lastCall = clock();
    if (! wasBypassed) engine.panic();
    wasBypassed = true;
    pendingRest = false;
    engine.setSettings (PhParams::read (apvts));
    sink.out = &midi;
    sink.last = juce::jmax (0, numSamples - 1);
    engine.flushOnly (numSamples, sink);
    publish();
}

void ParticleholdProcessor::publish()
{
    display.output.store ((float) engine.output());
    display.input.store ((float) engine.input());
    display.gate.store (engine.gate());
    display.tracking.store (engine.tracking());
    display.gliding.store (engine.gliding());
    display.rectified.store (engine.rectified());
    display.edges.store (engine.edges());
    for (int i = 0; i < particlehold::kNumSmoothOutputs; ++i) display.sent[i].store (engine.ccStream().lastSent (i));
    display.pulseOn.store (engine.pulses().isOn (particlehold::kClockIndex));
    display.pulseStarts.store (engine.pulses().starts (particlehold::kClockIndex));

    // New scope points (at most the last kScope).
    const auto made = engine.scopeCount();
    auto from = display.scopeCount.load();
    if (made > from + (unsigned long long) ParticleholdDisplay::kScope) from = made - (unsigned long long) ParticleholdDisplay::kScope;
    for (auto k = from; k < made; ++k)
    {
        const auto& p = engine.scopePoint (k);
        const auto slot = (size_t) (k % (unsigned long long) ParticleholdDisplay::kScope);
        display.scopeIn[slot].store (p.input);
        display.scopeOut[slot].store (p.output);
        display.scopeEdge[slot].store (p.edge);
    }
    display.scopeCount.store (made);
    display.blocks.fetch_add (1);
}

void ParticleholdProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    if (auto xml = state.createXml())
    {
        xml->setAttribute ("pluginVersion", JucePlugin_VersionString);
        copyXmlToBinary (*xml, destData);
    }
}

void ParticleholdProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
            // The Trigger button is a momentary button: a project never opens with it held.
            if (auto* p = apvts.getParameter (PhParams::trigger)) p->setValueNotifyingHost (0.0f);
        }
}

juce::AudioProcessorEditor* ParticleholdProcessor::createEditor() { return new ParticleholdEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ParticleholdProcessor(); }
