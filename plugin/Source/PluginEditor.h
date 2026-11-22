// The particlehold window, as approved in the draft (2026-11-22). Built the same way as Bryn's window.
//   left:  the scope (the last 4 s: the input in grey, the output in white above 0 V and amber below, a dot at each
//          sample moment); the signal controls (Source, Source CC, Level, Rectifier, Slew); the clock controls
//          (Clock, Rate, Range, the round Trigger button);
//   right: the Mode switch (Sample and hold | Hold and track); the present value (the module's LED: white above 0 V,
//          amber below, brighter further out) with "holding / tracking / gliding" and the clock and track lights;
//          the outputs (Signal, Noise, Clock) with their switches, numbers, level bars and lights; Send as;
//   top:   the name and the MIDI out channel; bottom right: the version.
// The panel is laid out at 780 x 470 and scaled to the window (80% to 200%, proportions locked).

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "Style.h"

class PhLookAndFeel : public juce::LookAndFeel_V4
{
public:
    PhLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float startAngle, float endAngle, juce::Slider&) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;
    void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override { return PhStyle::font (12.5f); }
    juce::Font getPopupMenuFont() override { return PhStyle::font (13.0f); }
};

// The scope: the last 4 seconds of the input and the output.
class ScopeView : public juce::Component, private juce::Timer
{
public:
    explicit ScopeView (ParticleholdProcessor& p);
    ~ScopeView() override { stopTimer(); }
    void paint (juce::Graphics&) override;

    static constexpr int kWidth = 440, kHeight = 180, kPoints = 400;   // 400 points of 10 ms = 4 s
    float yOf (float value) const { return 170.0f - value * 160.0f; }   // 0..1 to the drawing
    // For the checks: the newest point drawn (x < 0: nothing yet).
    juce::Point<float> newest() const;
    void refresh() { timerCallback(); }

private:
    void timerCallback() override;
    ParticleholdProcessor& proc;
    unsigned long long shownCount = ~0ull;
    bool shownRect = false;
};

// The present value: the module's LED (white above 0 V, amber below, brighter further out), the value as a number,
// what it is doing (holding, tracking, gliding), and the clock and track lights.
class NowView : public juce::Component, private juce::Timer
{
public:
    explicit NowView (ParticleholdProcessor& p);
    ~NowView() override { stopTimer(); }
    void paint (juce::Graphics&) override;
    static float volts (float output, bool rectified) { return rectified ? output : (output - 0.5f) * 2.0f; }   // -1 .. +1
    bool clockLit() const { return clockShown; }
    bool trackLit() const { return trackShown; }
    juce::String status() const;
    void refresh() { timerCallback(); }

private:
    void timerCallback() override;
    ParticleholdProcessor& proc;
    float shownOut = -1.0f;
    bool clockShown = false, trackShown = false, rectShown = false, glideShown = false;
    unsigned seenEdges = 0;
    int clockHold = 0;
};

// The round Trigger button: pressed = a clock pulse (in sample and hold a new value; in hold and track it tracks while
// held). Glows while the clock is high.
class TriggerButton : public juce::Component, private juce::Timer
{
public:
    explicit TriggerButton (ParticleholdProcessor& p);
    ~TriggerButton() override { stopTimer(); }
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void press (bool down);                         // also used by the checks

private:
    void timerCallback() override;
    ParticleholdProcessor& proc;
    juce::RangedAudioParameter* param;
    bool pressed = false, lit = false;
};

// A row of segments for a menu or switch control (Mode, Rectifier, Range, Send as).
class Segmented : public juce::Component
{
public:
    Segmented (juce::RangedAudioParameter& p, const juce::StringArray& names);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void choose (int index);                        // also used by the checks
    int selected() const { return index; }

private:
    juce::StringArray names;
    int index = 0;
    juce::ParameterAttachment attachment;
};

// The level bar of the Signal or Noise output (the value it last sent), drawn from 0 V.
class LevelBar : public juce::Component, private juce::Timer
{
public:
    LevelBar (ParticleholdProcessor& p, int output);
    ~LevelBar() override { stopTimer(); }
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    ParticleholdProcessor& proc;
    int output;
    int shownValue = -2;
    bool shownRect = false;
};

// The lights of the three outputs: Signal (white / amber, brighter further from 0 V), Noise (flickers while on),
// Clock (lit while on; a pulse shorter than the panel's refresh still flashes for about 100 ms).
class OutputLights : public juce::Component, private juce::Timer
{
public:
    explicit OutputLights (ParticleholdProcessor& p);
    ~OutputLights() override { stopTimer(); }
    void paint (juce::Graphics&) override;
    static constexpr int kRowStep = 36;
    float level (int k) const { return shown[k]; }   // for the checks: 0 = dark
    void refresh() { timerCallback(); }

private:
    void timerCallback() override;
    ParticleholdProcessor& proc;
    float shown[3] {};
    bool amberSignal = false;
    unsigned seenStarts = 0;
    int hold = 0;
};

// The whole panel at its design size (780 x 470). The editor below scales it to the window.
class ParticleholdPanel : public juce::Component, private juce::Timer
{
public:
    explicit ParticleholdPanel (ParticleholdProcessor&);
    ~ParticleholdPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // For the offline checks: every control and the parameter it moves.
    struct ControlInfo { juce::Component* control; juce::String paramId; };
    std::vector<ControlInfo> controls();
    TriggerButton& getTriggerButton() { return triggerButton; }
    OutputLights& getOutputLights() { return outputLights; }
    ScopeView& getScope() { return scope; }
    NowView& getNow() { return now; }
    Segmented& getModeSwitch() { return modeSwitch; }
    juce::String labelFor (const char* id) const;   // the text under a knob or switch ("level 100%", "rate 0.45 Hz")

    static constexpr int kWidth = 780, kHeight = 470;
    static constexpr int kRowY[3] = { 236, 272, 308 };   // Signal, Noise, Clock

private:
    void timerCallback() override;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    ParticleholdProcessor& proc;
    PhLookAndFeel lnf;

    ScopeView scope;
    juce::ComboBox sourceBox, sourceCcBox, clockBox, channelBox;
    juce::Slider levelKnob, slewKnob, rateKnob;
    Segmented rectifySwitch, rangeSwitch, modeSwitch, sendAs;
    TriggerButton triggerButton;
    NowView now;
    juce::ToggleButton outOn[3];
    LevelBar signalBar, noiseBar;
    OutputLights outputLights;

    std::unique_ptr<SliderAttachment> levelAtt, slewAtt, rateAtt;
    std::unique_ptr<ComboAttachment> sourceAtt, sourceCcAtt, clockAtt, channelAtt;
    std::unique_ptr<ButtonAttachment> outOnAtt[3];
    juce::String shownLabels;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParticleholdPanel)
};

// The plugin window: resizable between 80% and 200% of the design size, proportions locked. The width is saved with
// the project.
class ParticleholdEditor : public juce::AudioProcessorEditor
{
public:
    explicit ParticleholdEditor (ParticleholdProcessor&);
    ~ParticleholdEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kWidth = ParticleholdPanel::kWidth, kHeight = ParticleholdPanel::kHeight;
    static constexpr int kMinWidth = 624, kMaxWidth = 1560;     // 80% and 200%
    static inline const juce::Identifier widthProperty { "editorWidth" };
    static int heightForWidth (int width) { return juce::roundToInt ((double) width * kHeight / kWidth); }

    ParticleholdPanel panel;

private:
    ParticleholdProcessor& proc;
    juce::ComponentBoundsConstrainer constrainer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParticleholdEditor)
};
