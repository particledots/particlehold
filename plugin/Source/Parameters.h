// Every control of particlehold, as Logic sees it.
//
// IMPORTANT: these ids, their order, ranges and menu lists were fixed in the first build (0.1.0) and must never
// change. Saved projects and automation refer to them.
// If something must be added one day, add it at the END with ParameterID version 2.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <particlehold/Engine.h>

namespace PhParams
{
    inline constexpr const char* mode       = "mode";        // Sample and hold / Hold and track
    inline constexpr const char* level      = "level";       // 0 .. 100 %
    inline constexpr const char* rectify    = "rectify";     // the rectifier, off / on
    inline constexpr const char* slew       = "slew";        // Off, 5 ms .. 10 s
    inline constexpr const char* rate       = "rate";        // 0 .. 100 % (Hz for the Range)
    inline constexpr const char* range      = "range";       // Low / High
    inline constexpr const char* clock      = "clock";       // Internal / Notes / Button only
    inline constexpr const char* source     = "source";      // Noise / CC / Aftertouch / Pitch bend / Velocity
    inline constexpr const char* sourceCc   = "sourceCc";    // 0 .. 127
    inline constexpr const char* trigger    = "trigger";     // the panel's Trigger button (never saved as held)
    inline constexpr const char* outChannel = "outChannel";  // 1 .. 16
    inline constexpr const char* signalOn   = "signalOn";    // Signal output on
    inline constexpr const char* noiseOn    = "noiseOn";     // Noise output on
    inline constexpr const char* clockOn    = "clockOn";     // Clock output on
    inline constexpr const char* sendAs     = "sendAs";      // CC / Note / Both

    inline constexpr float kSlewMinMs = 5.0f, kSlewMaxMs = 10000.0f;

    extern const char* const outputOnIds[3];       // signalOn, noiseOn, clockOn
    extern const juce::StringArray modeNames;      // Sample and hold, Hold and track
    extern const juce::StringArray rangeNames;     // Low, High
    extern const juce::StringArray clockNames;     // Internal, Notes, Button only
    extern const juce::StringArray sourceNames;    // Noise, CC, Aftertouch, Pitch bend, Velocity
    extern const juce::StringArray sendAsNames;    // CC, Note, Both

    juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // The current values, for the engine.
    particlehold::Settings read (const juce::AudioProcessorValueTreeState& apvts);

    juce::String timeText (float ms);   // "Off", "5.0 ms", "300 ms", "1.20 s"
    juce::String hzText (double hz);    // "0.45 Hz", "12 Hz"
    juce::String ccName (int number);   // "CC 1 mod wheel"
}
