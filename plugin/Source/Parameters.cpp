#include "Parameters.h"

#include <cmath>

namespace PhParams
{
const char* const outputOnIds[3] = { signalOn, noiseOn, clockOn };
const juce::StringArray modeNames   { "Sample and hold", "Hold and track" };
const juce::StringArray rangeNames  { "Low", "High" };
const juce::StringArray clockNames  { "Internal", "Notes", "Button only" };
const juce::StringArray sourceNames { "Noise", "CC", "Aftertouch", "Pitch bend", "Velocity" };
const juce::StringArray sendAsNames { "CC", "Note", "Both" };

juce::String timeText (float ms)
{
    if (ms < kSlewMinMs * 0.5f) return "Off";
    if (ms < 10.0f)    return juce::String (ms, 1) + " ms";
    if (ms < 1000.0f)  return juce::String (juce::roundToInt (ms)) + " ms";
    return juce::String (ms / 1000.0f, 2) + " s";
}

juce::String hzText (double hz)
{
    if (hz < 1.0)  return juce::String (hz, 2) + " Hz";
    if (hz < 10.0) return juce::String (hz, 1) + " Hz";
    return juce::String (juce::roundToInt (hz)) + " Hz";
}

juce::String ccName (int n)
{
    juce::String name;
    switch (n)
    {
        case 0: name = "bank select"; break;   case 1: name = "mod wheel"; break;      case 2: name = "breath"; break;
        case 3: name = "Hillside envelope"; break;  case 4: name = "foot"; break;      case 7: name = "volume"; break;
        case 9: name = "Hillside linear"; break;    case 10: name = "pan"; break;      case 11: name = "expression"; break;
        case 64: name = "sustain"; break;       case 74: name = "brightness"; break;
        case 75: name = "particlehold Signal"; break;  case 77: name = "particlehold Noise"; break;
        case 80: name = "Bryn Function"; break;
        default: break;
    }
    if (n >= 20 && n <= 27) name = "Ogdoad LFO " + juce::String (n - 19);
    const char* drift[] = { "ripple X", "ripple Y", "tide X", "tide Y", "glacier X", "glacier Y", "blend" };
    if (n >= 106 && n <= 112) name = juce::String ("Strange Drift ") + drift[n - 106];
    return "CC " + juce::String (n) + (name.isEmpty() ? juce::String() : " " + name);
}

namespace
{
    using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

    void addBool (Layout& l, const char* id, const char* name, bool def)
    {
        l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def));
    }
    void addChoice (Layout& l, const char* id, const char* name, const juce::StringArray& names, int def)
    {
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, names, def));
    }
    void addPercent (Layout& l, const char* id, const char* name, float def)
    {
        l.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { id, 1 }, name, juce::NormalisableRange<float> (0.0f, 100.0f, 0.0f), def,
            juce::AudioParameterFloatAttributes()
                .withLabel ("%")
                .withStringFromValueFunction ([] (float v, int) { return juce::String (juce::roundToInt (v)) + "%"; })
                .withValueFromStringFunction ([] (const juce::String& t) { return t.getFloatValue(); })));
    }
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    Layout l;
    addChoice (l, mode, "Mode", modeNames, particlehold::modeSampleHold);
    addPercent (l, level, "Level", 100.0f);
    addBool (l, rectify, "Rectifier", false);
    {
        // The slew: the knob's far left is Off; from there it moves the time on a logarithmic scale, 5 ms to 10 s
        // (equal ratios for equal turns; the centre is 224 ms).
        juce::NormalisableRange<float> r (0.0f, kSlewMaxMs,
            [] (float, float, float x) { return x <= 0.0f ? 0.0f : kSlewMinMs * std::pow (kSlewMaxMs / kSlewMinMs, juce::jlimit (0.0f, 1.0f, x)); },
            [] (float, float, float v) {
                if (v < kSlewMinMs * 0.5f) return 0.0f;
                return juce::jmax (1.0e-4f, std::log (juce::jlimit (kSlewMinMs, kSlewMaxMs, v) / kSlewMinMs) / std::log (kSlewMaxMs / kSlewMinMs)); });
        l.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { slew, 1 }, "Slew", r, 0.0f,
            juce::AudioParameterFloatAttributes()
                .withLabel ("ms")
                .withStringFromValueFunction ([] (float v, int) { return timeText (v); })
                .withValueFromStringFunction ([] (const juce::String& t) {
                    if (t.trim().equalsIgnoreCase ("off")) return 0.0f;
                    const float x = t.getFloatValue();
                    return t.containsIgnoreCase ("ms") || ! t.containsIgnoreCase ("s") ? x : x * 1000.0f; })));
    }
    addPercent (l, rate, "Rate", 50.0f);
    addChoice (l, range, "Range", rangeNames, 0);
    addChoice (l, clock, "Clock", clockNames, particlehold::clockNotes);
    addChoice (l, source, "Source", sourceNames, particlehold::sourceNoise);
    l.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { sourceCc, 1 }, "Source CC", 0, 127, 1));
    addBool (l, trigger, "Trigger button", false);
    l.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { outChannel, 1 }, "MIDI out channel", 1, 16, particlehold::kDefaultChannel));
    addBool (l, signalOn, "Signal out", true);
    addBool (l, noiseOn, "Noise out", false);
    addBool (l, clockOn, "Clock out", true);
    addChoice (l, sendAs, "Send as", sendAsNames, 0);
    return l;
}

particlehold::Settings read (const juce::AudioProcessorValueTreeState& apvts)
{
    auto v = [&] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    particlehold::Settings s;
    s.mode       = juce::jlimit (0, 1, juce::roundToInt (v (mode)));
    s.level      = juce::jlimit (0.0, 1.0, (double) v (level) / 100.0);
    s.rectify    = v (rectify) >= 0.5f;
    s.slew       = v (slew) < kSlewMinMs * 0.5f ? 0.0 : (double) v (slew) / 1000.0;
    s.rateHz     = particlehold::rateHz ((double) v (rate) / 100.0, juce::roundToInt (v (range)) == 1);
    s.clock      = juce::jlimit (0, 2, juce::roundToInt (v (clock)));
    s.source     = juce::jlimit (0, 4, juce::roundToInt (v (source)));
    s.sourceCc   = juce::jlimit (0, 127, juce::roundToInt (v (sourceCc)));
    s.outChannel = juce::jlimit (1, 16, juce::roundToInt (v (outChannel)));
    s.outOn[particlehold::outSignal] = v (signalOn) >= 0.5f;
    s.outOn[particlehold::outNoise]  = v (noiseOn) >= 0.5f;
    s.pulseOn[particlehold::kClockIndex] = v (clockOn) >= 0.5f;
    s.sendAs     = juce::jlimit (0, 2, juce::roundToInt (v (sendAs)));
    return s;
}
}
