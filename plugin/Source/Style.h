// particlehold's own look (as in the approved draft, 2026-11-22): a dark graphite panel; white for values above 0 V
// and amber below (the module's two LED colours), white switches. Original; nothing taken from any module maker's
// artwork. Same structure as Bryn's Style.h.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace PhStyle
{
    inline const juce::Colour panel     { 0xff17191c };
    inline const juce::Colour display   { 0xff0f1113 };
    inline const juce::Colour line      { 0xff2a2e33 };
    inline const juce::Colour line2     { 0xff353a40 };
    inline const juce::Colour knob      { 0xff22262a };
    inline const juce::Colour selected  { 0xff33393f };
    inline const juce::Colour text      { 0xffc8ccd2 };
    inline const juce::Colour bright    { 0xffe8eef2 };   // "ice": values above 0 V, switches on
    inline const juce::Colour dim       { 0xff7d848c };
    inline const juce::Colour faint     { 0xff5a6068 };
    inline const juce::Colour trace     { 0xff30353b };   // the input on the scope
    inline const juce::Colour amber     { 0xfff0a83c };   // values below 0 V, the clock and the Trigger button
    inline const juce::Colour amberDeep { 0xff3d3220 };

    inline juce::Font font (float size) { return juce::Font (juce::FontOptions (size)); }
}
