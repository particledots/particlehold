#include "PluginEditor.h"
#include "Parameters.h"

#include <cmath>

using namespace PhStyle;

namespace
{
    float param (const ParticleholdProcessor& p, const char* id)
    {
        return const_cast<ParticleholdProcessor&> (p).getValueTreeState().getRawParameterValue (id)->load();
    }
    int choice (const ParticleholdProcessor& p, const char* id) { return juce::roundToInt (param (p, id)); }

    // A round light with a soft halo; level 0 to 1 decides how brightly it shines. (As Bryn's.)
    void drawLight (juce::Graphics& g, float cx, float cy, float r, juce::Colour c, float level)
    {
        level = juce::jlimit (0.0f, 1.0f, level);
        if (level > 0.15f)
        {
            g.setColour (c.withAlpha (0.12f * level));
            g.fillEllipse (cx - r - 4.0f, cy - r - 4.0f, 2.0f * (r + 4.0f), 2.0f * (r + 4.0f));
        }
        g.setColour (knob);
        g.fillEllipse (cx - r, cy - r, 2.0f * r, 2.0f * r);
        if (level <= 0.0f)
        {
            g.setColour (line2);
            g.drawEllipse (cx - r, cy - r, 2.0f * r, 2.0f * r, 1.0f);
        }
        g.setColour (c.withAlpha (0.12f + 0.88f * level));
        g.fillEllipse (cx - r, cy - r, 2.0f * r, 2.0f * r);
    }

    juce::Colour signColour (float volts) { return volts < 0.0f ? amber : bright; }
}

// ================================================================================================ look and feel
PhLookAndFeel::PhLookAndFeel()
{
    setColour (juce::ComboBox::textColourId, bright);
    setColour (juce::ComboBox::arrowColourId, dim);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff23262b));
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, selected);
    setColour (juce::PopupMenu::highlightedTextColourId, bright);
    setColour (juce::Label::textColourId, text);
    setColour (juce::Slider::textBoxTextColourId, dim);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
}

void PhLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider&)
{
    const float r = (float) juce::jmin (w, h) * 0.5f - 4.0f;
    const float cx = (float) x + (float) w * 0.5f, cy = (float) y + (float) h * 0.5f;
    const float angle = a0 + pos * (a1 - a0);
    // The track around the knob, and the part turned so far.
    juce::Path track, done;
    track.addCentredArc (cx, cy, r + 3.0f, r + 3.0f, 0.0f, a0, a1, true);
    g.setColour (line);
    g.strokePath (track, juce::PathStrokeType (2.0f));
    if (pos > 0.0f)
    {
        done.addCentredArc (cx, cy, r + 3.0f, r + 3.0f, 0.0f, a0, angle, true);
        g.setColour (bright.withAlpha (0.55f));
        g.strokePath (done, juce::PathStrokeType (2.0f));
    }
    g.setColour (knob);
    g.fillEllipse (cx - r, cy - r, 2.0f * r, 2.0f * r);
    g.setColour (line2);
    g.drawEllipse (cx - r, cy - r, 2.0f * r, 2.0f * r, 1.0f);
    const float inner = r * 0.25f, outer = r * 0.85f;
    g.setColour (bright);
    g.drawLine (cx + inner * std::sin (angle), cy - inner * std::cos (angle), cx + outer * std::sin (angle), cy - outer * std::cos (angle), 2.5f);
}

void PhLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool, bool)
{
    const auto r = b.getLocalBounds().toFloat().withSizeKeepingCentre (30.0f, 18.0f);
    const bool on = b.getToggleState();
    g.setColour (on ? bright : knob);
    g.fillRoundedRectangle (r, 9.0f);
    if (! on)
    {
        g.setColour (line2);
        g.drawRoundedRectangle (r, 9.0f, 1.0f);
    }
    const float kx = on ? r.getRight() - 9.0f : r.getX() + 9.0f;
    g.setColour (on ? panel : dim);
    g.fillEllipse (kx - 6.0f, r.getCentreY() - 6.0f, 12.0f, 12.0f);
}

void PhLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox&)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (0.5f);
    g.setColour (display);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (line2);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
    juce::Path arrow;
    const float ax = (float) w - 10.0f, ay = (float) h * 0.5f;
    arrow.addTriangle (ax - 3.5f, ay - 2.0f, ax + 3.5f, ay - 2.0f, ax, ay + 2.5f);
    g.setColour (dim);
    g.fillPath (arrow);
}

void PhLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (5, 1, box.getWidth() - 20, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
    label.setMinimumHorizontalScale (0.6f);
}

// ================================================================================================ scope
ScopeView::ScopeView (ParticleholdProcessor& p) : proc (p)
{
    setInterceptsMouseClicks (false, false);
    startTimerHz (30);
}

void ScopeView::timerCallback()
{
    const auto c = proc.getDisplay().scopeCount.load();
    const bool rect = proc.getDisplay().rectified.load();
    if (c != shownCount || rect != shownRect) { shownCount = c; shownRect = rect; repaint(); }
}

juce::Point<float> ScopeView::newest() const
{
    const auto& d = proc.getDisplay();
    const auto c = d.scopeCount.load();
    if (c == 0) return { -1.0f, -1.0f };
    const auto slot = (size_t) ((c - 1) % (unsigned long long) ParticleholdDisplay::kScope);
    return { 430.0f, yOf (d.scopeOut[slot].load()) };
}

void ScopeView::paint (juce::Graphics& g)
{
    const auto& d = proc.getDisplay();
    const auto box = getLocalBounds().toFloat();
    g.setColour (display);
    g.fillRoundedRectangle (box, 6.0f);
    g.setColour (line);
    g.drawRoundedRectangle (box.reduced (0.5f), 6.0f, 1.0f);

    const bool rect = d.rectified.load();
    const float zero = rect ? 0.0f : 0.5f;
    const float y0 = yOf (zero);
    {
        juce::Path z;
        z.startNewSubPath (10.0f, y0);
        z.lineTo ((float) kWidth - 10.0f, y0);
        juce::Path dashed;
        const float dashes[] = { 4.0f, 4.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, z, dashes, 2);
        g.setColour (line2);
        g.fillPath (dashed);
    }
    g.setFont (font (11.0f));
    g.setColour (dim);
    g.drawText ("+", 10, 6, 20, 14, juce::Justification::centredLeft);
    g.drawText ("0 V", 10, (int) y0 - (rect ? 16 : 15), 40, 14, juce::Justification::centredLeft);
    if (! rect) g.drawText (juce::String::fromUTF8 ("\xe2\x88\x92"), 10, kHeight - 20, 20, 14, juce::Justification::centredLeft);
    g.drawText (juce::String::fromUTF8 ("last 4 s  \xc2\xb7  grey: input  \xc2\xb7  white / amber: output"), kWidth - 300, 6, 290, 14, juce::Justification::centredRight);

    const auto count = d.scopeCount.load();
    if (count == 0) return;
    const int n = (int) juce::jmin ((unsigned long long) kPoints, count);
    const float step = 420.0f / (float) (kPoints - 1);
    auto slotOf = [&] (int i) { return (size_t) ((count - (unsigned long long) (n - i)) % (unsigned long long) ParticleholdDisplay::kScope); };
    auto xOf = [&] (int i) { return 10.0f + (float) (kPoints - n + i) * step; };

    juce::Path in, outPath;
    for (int i = 0; i < n; ++i)
    {
        const auto s = slotOf (i);
        const float xi = xOf (i);
        const float yi = yOf (d.scopeIn[s].load()), yo = yOf (d.scopeOut[s].load());
        if (i == 0) { in.startNewSubPath (xi, yi); outPath.startNewSubPath (xi, yo); }
        else        { in.lineTo (xi, yi); outPath.lineTo (xi, yo); }
    }
    g.setColour (trace);
    g.strokePath (in, juce::PathStrokeType (1.0f));
    // The output: white above 0 V, amber below.
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (0, 0, kWidth, (int) std::ceil (y0));
        g.setColour (bright);
        g.strokePath (outPath, juce::PathStrokeType (2.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
    }
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (0, (int) std::floor (y0), kWidth, kHeight);
        g.setColour (amber);
        g.strokePath (outPath, juce::PathStrokeType (2.0f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
    }
    // A dot at each sample moment.
    for (int i = 0; i < n; ++i)
    {
        const auto s = slotOf (i);
        if (! d.scopeEdge[s].load()) continue;
        const float v = d.scopeOut[s].load();
        g.setColour (signColour (NowView::volts (v, rect)));
        g.fillEllipse (xOf (i) - 3.5f, yOf (v) - 3.5f, 7.0f, 7.0f);
    }
}

// ================================================================================================ the present value
NowView::NowView (ParticleholdProcessor& p) : proc (p)
{
    setInterceptsMouseClicks (false, false);
    seenEdges = proc.getDisplay().edges.load();
    startTimerHz (30);
}

juce::String NowView::status() const
{
    const auto& d = proc.getDisplay();
    if (d.tracking.load()) return "tracking";
    if (d.gliding.load())  return "gliding";
    return "holding";
}

void NowView::timerCallback()
{
    const auto& d = proc.getDisplay();
    const unsigned e = d.edges.load();
    if (e != seenEdges) { seenEdges = e; clockHold = 3; }
    else if (clockHold > 0) --clockHold;
    const bool clk = d.gate.load() || clockHold > 0;
    const float o = std::round (d.output.load() * 500.0f) / 500.0f;
    const bool tr = d.tracking.load(), rc = d.rectified.load(), gl = d.gliding.load();
    if (clk != clockShown || tr != trackShown || rc != rectShown || gl != glideShown || ! juce::exactlyEqual (o, shownOut))
    {
        clockShown = clk; trackShown = tr; rectShown = rc; glideShown = gl; shownOut = o;
        repaint();
    }
}

void NowView::paint (juce::Graphics& g)
{
    const auto& d = proc.getDisplay();
    const auto box = getLocalBounds().toFloat();
    g.setColour (display);
    g.fillRoundedRectangle (box, 6.0f);
    g.setColour (line);
    g.drawRoundedRectangle (box.reduced (0.5f), 6.0f, 1.0f);

    const float v = volts (d.output.load(), d.rectified.load());
    drawLight (g, 42.0f, 48.0f, 22.0f, signColour (v), std::fabs (v));
    g.setColour (bright);
    g.setFont (font (24.0f));
    g.drawText ((v >= 0.0f ? "+" : juce::String::fromUTF8 ("\xe2\x88\x92")) + juce::String (std::fabs (v), 2), 78, 22, 120, 30, juce::Justification::centredLeft);
    g.setColour (dim);
    g.setFont (font (12.0f));
    g.drawText (status(), 78, 54, 120, 16, juce::Justification::centredLeft);

    drawLight (g, 256.0f, 30.0f, 6.0f, amber, clockShown ? 1.0f : 0.0f);
    drawLight (g, 256.0f, 66.0f, 6.0f, amber, trackShown ? 1.0f : 0.0f);
    g.setFont (font (12.0f));
    g.setColour (clockShown ? text : dim);
    g.drawText ("clock", 196, 22, 50, 16, juce::Justification::centredRight);
    g.setColour (trackShown ? text : dim);
    g.drawText ("track", 196, 58, 50, 16, juce::Justification::centredRight);
}

// ================================================================================================ Trigger button
TriggerButton::TriggerButton (ParticleholdProcessor& p)
    : proc (p), param (p.getValueTreeState().getParameter (PhParams::trigger))
{
    startTimerHz (30);
}

void TriggerButton::press (bool down)
{
    if (down == pressed) return;
    pressed = down;
    if (down) { param->beginChangeGesture(); param->setValueNotifyingHost (1.0f); }
    else      { param->setValueNotifyingHost (0.0f); param->endChangeGesture(); }
    repaint();
}

void TriggerButton::mouseDown (const juce::MouseEvent&) { press (true); }
void TriggerButton::mouseUp (const juce::MouseEvent&)   { press (false); }

void TriggerButton::timerCallback()
{
    const bool l = proc.getDisplay().gate.load();
    if (l != lit) { lit = l; repaint(); }
}

void TriggerButton::paint (juce::Graphics& g)
{
    const float r = 23.0f, cx = 24.0f, cy = 24.0f;
    g.setColour (knob);
    g.fillEllipse (cx - r, cy - r, 2.0f * r, 2.0f * r);
    g.setColour (amber.withAlpha (lit ? 1.0f : 0.5f));
    g.drawEllipse (cx - r, cy - r, 2.0f * r, 2.0f * r, 1.5f);
    const float ri = 15.0f;
    g.setColour (pressed ? amberDeep.brighter (0.6f) : (lit ? juce::Colour (0xff6a5024) : amberDeep));
    g.fillEllipse (cx - ri, cy - ri, 2.0f * ri, 2.0f * ri);
}

// ================================================================================================ segmented switch
Segmented::Segmented (juce::RangedAudioParameter& p, const juce::StringArray& n)
    : names (n), attachment (p, [this] (float v) { index = juce::roundToInt (v); repaint(); })
{
    attachment.sendInitialUpdate();
}

void Segmented::choose (int i)
{
    i = juce::jlimit (0, names.size() - 1, i);
    attachment.setValueAsCompleteGesture ((float) i);
}

void Segmented::mouseDown (const juce::MouseEvent& e)
{
    if (! isEnabled()) return;
    choose ((int) ((float) e.x / ((float) getWidth() / (float) names.size())));
}

void Segmented::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (display);
    g.fillRoundedRectangle (r, 5.0f);
    g.setColour (line2);
    g.drawRoundedRectangle (r, 5.0f, 1.0f);
    const float w = r.getWidth() / (float) names.size();
    for (int i = 0; i < names.size(); ++i)
    {
        const auto seg = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (3.0f);
        if (i == index)
        {
            g.setColour (PhStyle::selected);
            g.fillRoundedRectangle (seg, 3.0f);
        }
        g.setColour (i == index ? bright : dim);
        g.setFont (font (r.getHeight() > 28.0f ? 14.0f : 12.5f));
        g.drawText (names[i], seg, juce::Justification::centred);
    }
}

// ================================================================================================ level bar
LevelBar::LevelBar (ParticleholdProcessor& p, int o) : proc (p), output (o)
{
    setInterceptsMouseClicks (false, false);
    startTimerHz (30);
}

void LevelBar::timerCallback()
{
    const int v = proc.getDisplay().sent[output].load();
    const bool rc = proc.getDisplay().rectified.load();
    if (v != shownValue || rc != shownRect) { shownValue = v; shownRect = rc; repaint(); }
}

void LevelBar::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (knob);
    g.fillRoundedRectangle (r, 2.0f);
    const int v = proc.getDisplay().sent[output].load();
    if (v < 0) return;
    const bool rect = output == particlehold::outSignal && proc.getDisplay().rectified.load();
    const float value = (float) v / 16383.0f, zero = rect ? 0.0f : 0.5f;
    const float x0 = r.getX() + r.getWidth() * zero, x1 = r.getX() + r.getWidth() * value;
    g.setColour (signColour (NowView::volts (value, rect)));
    g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (x0, x1), r.getY(), juce::jmax (2.0f, std::fabs (x1 - x0)), r.getHeight()), 2.0f);
}

// ================================================================================================ output lights
OutputLights::OutputLights (ParticleholdProcessor& p) : proc (p)
{
    setInterceptsMouseClicks (false, false);
    seenStarts = proc.getDisplay().pulseStarts.load();
    startTimerHz (30);
}

void OutputLights::timerCallback()
{
    const auto& d = proc.getDisplay();
    auto& vts = proc.getValueTreeState();
    auto on = [&] (const char* id) { return vts.getRawParameterValue (id)->load() >= 0.5f; };
    float now[3] {};
    const bool rect = d.rectified.load();
    const int sig = d.sent[particlehold::outSignal].load(), nz = d.sent[particlehold::outNoise].load();
    bool amb = false;
    if (on (PhParams::signalOn) && sig >= 0)
    {
        const float v = NowView::volts ((float) sig / 16383.0f, rect);
        now[0] = juce::jmax (0.08f, std::fabs (v));
        amb = v < 0.0f;
    }
    if (on (PhParams::noiseOn) && nz >= 0) now[1] = juce::jmax (0.15f, std::fabs ((float) nz / 16383.0f - 0.5f) * 2.0f);
    const unsigned st = d.pulseStarts.load();
    if (st != seenStarts) { seenStarts = st; hold = 3; }
    else if (hold > 0) --hold;
    now[2] = d.pulseOn.load() || hold > 0 ? 1.0f : 0.0f;
    now[0] = std::round (now[0] * 20.0f) / 20.0f;
    now[1] = std::round (now[1] * 20.0f) / 20.0f;
    bool changed = amb != amberSignal;
    for (int k = 0; k < 3; ++k) changed = changed || ! juce::exactlyEqual (now[k], shown[k]);
    if (changed)
    {
        for (int k = 0; k < 3; ++k) shown[k] = now[k];
        amberSignal = amb;
        repaint();
    }
}

void OutputLights::paint (juce::Graphics& g)
{
    drawLight (g, 10.0f, 10.0f, 5.0f, amberSignal ? amber : bright, shown[0]);
    drawLight (g, 10.0f, 10.0f + (float) kRowStep, 5.0f, bright, shown[1]);
    drawLight (g, 10.0f, 10.0f + 2.0f * (float) kRowStep, 5.0f, amber, shown[2]);
}

// ================================================================================================ panel
ParticleholdPanel::ParticleholdPanel (ParticleholdProcessor& p)
    : proc (p), scope (p),
      rectifySwitch (*p.getValueTreeState().getParameter (PhParams::rectify), { "off", "on" }),
      rangeSwitch (*p.getValueTreeState().getParameter (PhParams::range), { "low", "high" }),
      modeSwitch (*p.getValueTreeState().getParameter (PhParams::mode), { "sample and hold", "hold and track" }),
      sendAs (*p.getValueTreeState().getParameter (PhParams::sendAs), PhParams::sendAsNames),
      triggerButton (p), now (p),
      signalBar (p, particlehold::outSignal), noiseBar (p, particlehold::outNoise),
      outputLights (p)
{
    setLookAndFeel (&lnf);
    auto& vts = proc.getValueTreeState();

    addAndMakeVisible (scope);
    sourceBox.addItemList (PhParams::sourceNames, 1);
    for (int n = 0; n < 128; ++n) sourceCcBox.addItem (PhParams::ccName (n), n + 1);
    clockBox.addItemList (PhParams::clockNames, 1);
    for (int ch = 1; ch <= 16; ++ch) channelBox.addItem (juce::String (ch), ch);
    for (auto* b : { &sourceBox, &sourceCcBox, &clockBox, &channelBox }) addAndMakeVisible (*b);
    sourceAtt   = std::make_unique<ComboAttachment> (vts, PhParams::source, sourceBox);
    sourceCcAtt = std::make_unique<ComboAttachment> (vts, PhParams::sourceCc, sourceCcBox);
    clockAtt    = std::make_unique<ComboAttachment> (vts, PhParams::clock, clockBox);
    channelAtt  = std::make_unique<ComboAttachment> (vts, PhParams::outChannel, channelBox);

    for (auto* k : { &levelKnob, &slewKnob, &rateKnob })
    {
        k->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        k->setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        k->setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        addAndMakeVisible (*k);
    }
    levelKnob.setName (PhParams::level); slewKnob.setName (PhParams::slew); rateKnob.setName (PhParams::rate);
    levelAtt = std::make_unique<SliderAttachment> (vts, PhParams::level, levelKnob);
    slewAtt  = std::make_unique<SliderAttachment> (vts, PhParams::slew, slewKnob);
    rateAtt  = std::make_unique<SliderAttachment> (vts, PhParams::rate, rateKnob);
    levelKnob.setDoubleClickReturnValue (true, 100.0);
    slewKnob.setDoubleClickReturnValue (true, 0.0);
    rateKnob.setDoubleClickReturnValue (true, 50.0);

    for (auto* s : { &rectifySwitch, &rangeSwitch, &modeSwitch, &sendAs }) addAndMakeVisible (*s);
    addAndMakeVisible (triggerButton);
    addAndMakeVisible (now);
    for (int k = 0; k < 3; ++k)
    {
        addAndMakeVisible (outOn[k]);
        outOnAtt[k] = std::make_unique<ButtonAttachment> (vts, PhParams::outputOnIds[k], outOn[k]);
    }
    addAndMakeVisible (signalBar);
    addAndMakeVisible (noiseBar);
    addAndMakeVisible (outputLights);
    startTimerHz (10);
    timerCallback();
}

ParticleholdPanel::~ParticleholdPanel() { stopTimer(); setLookAndFeel (nullptr); }

std::vector<ParticleholdPanel::ControlInfo> ParticleholdPanel::controls()
{
    return {
        { &modeSwitch, PhParams::mode }, { &levelKnob, PhParams::level }, { &rectifySwitch, PhParams::rectify },
        { &slewKnob, PhParams::slew }, { &rateKnob, PhParams::rate }, { &rangeSwitch, PhParams::range },
        { &clockBox, PhParams::clock }, { &sourceBox, PhParams::source }, { &sourceCcBox, PhParams::sourceCc },
        { &triggerButton, PhParams::trigger }, { &channelBox, PhParams::outChannel },
        { &outOn[0], PhParams::signalOn }, { &outOn[1], PhParams::noiseOn }, { &outOn[2], PhParams::clockOn },
        { &sendAs, PhParams::sendAs },
    };
}

juce::String ParticleholdPanel::labelFor (const char* id) const
{
    auto& vts = proc.getValueTreeState();
    const juce::String key (id);
    if (key == PhParams::level) return "level " + vts.getParameter (PhParams::level)->getCurrentValueAsText();
    if (key == PhParams::slew)  return "slew " + PhParams::timeText (param (proc, PhParams::slew));
    if (key == PhParams::rate)  return "rate " + PhParams::hzText (particlehold::rateHz (param (proc, PhParams::rate) / 100.0, choice (proc, PhParams::range) == 1));
    return {};
}

void ParticleholdPanel::timerCallback()
{
    // The texts under the knobs and beside the Clock output follow the controls; the Source CC menu is dimmed unless
    // the Source is CC.
    const juce::String labels = labelFor (PhParams::level) + labelFor (PhParams::slew) + labelFor (PhParams::rate)
                              + juce::String (choice (proc, PhParams::sendAs)) + juce::String (choice (proc, PhParams::source));
    if (labels != shownLabels)
    {
        shownLabels = labels;
        sourceCcBox.setAlpha (choice (proc, PhParams::source) == particlehold::sourceCc ? 1.0f : 0.45f);
        repaint();
    }
}

void ParticleholdPanel::resized()
{
    scope.setBounds (20, 56, ScopeView::kWidth, ScopeView::kHeight);
    sourceBox.setBounds (20, 264, 130, 26);
    sourceCcBox.setBounds (20, 298, 130, 26);
    levelKnob.setBounds (176, 262, 60, 60);
    rectifySwitch.setBounds (258, 279, 86, 26);
    slewKnob.setBounds (382, 262, 60, 60);
    clockBox.setBounds (20, 382, 130, 26);
    rateKnob.setBounds (176, 372, 60, 60);
    rangeSwitch.setBounds (258, 389, 86, 26);
    triggerButton.setBounds (388, 378, 48, 48);

    channelBox.setBounds (676, 15, 48, 24);
    modeSwitch.setBounds (480, 56, 280, 32);
    now.setBounds (480, 100, 280, 96);
    for (int k = 0; k < 3; ++k) outOn[k].setBounds (480, kRowY[k] - 10, 34, 20);
    signalBar.setBounds (680, kRowY[0] - 5, 56, 10);
    noiseBar.setBounds (680, kRowY[1] - 5, 56, 10);
    outputLights.setBounds (742, kRowY[0] - 10, 20, 20 + 2 * OutputLights::kRowStep);
    sendAs.setBounds (540, 342, 220, 26);
}

void ParticleholdPanel::paint (juce::Graphics& g)
{
    g.fillAll (panel);

    // Header.
    g.setColour (bright);
    g.setFont (font (24.0f));
    g.drawText ("particlehold", 20, 10, 150, 28, juce::Justification::centredLeft);
    g.setColour (dim);
    g.setFont (font (12.0f));
    g.drawText (juce::String::fromUTF8 ("particledots  \xc2\xb7  sample and hold"), 166, 17, 260, 18, juce::Justification::centredLeft);
    g.drawText ("MIDI out ch", 590, 17, 82, 18, juce::Justification::centredRight);
    g.setColour (line);
    g.drawHorizontalLine (46, 20.0f, 760.0f);
    g.drawVerticalLine (470, 56.0f, 452.0f);
    g.drawHorizontalLine (356, 20.0f, 460.0f);

    // Section names and the texts under the controls.
    g.setColour (dim);
    g.setFont (font (12.0f));
    g.drawText ("signal", 20, 244, 100, 16, juce::Justification::centredLeft);
    g.drawText ("clock", 20, 362, 100, 16, juce::Justification::centredLeft);
    g.setColour (text);
    g.drawText (labelFor (PhParams::level), 146, 326, 120, 16, juce::Justification::centred);
    g.drawText ("rectifier", 258, 326, 86, 16, juce::Justification::centred);
    g.drawText (labelFor (PhParams::slew), 352, 326, 120, 16, juce::Justification::centred);
    g.drawText (labelFor (PhParams::rate), 146, 436, 120, 16, juce::Justification::centred);
    g.drawText ("range", 258, 436, 86, 16, juce::Justification::centred);
    g.drawText ("trigger", 372, 436, 80, 16, juce::Justification::centred);

    // Outputs.
    g.setColour (dim);
    g.drawText ("outputs", 480, 206, 100, 16, juce::Justification::centredLeft);
    const int mode = choice (proc, PhParams::sendAs);
    const char* names[3] = { "Signal", "Noise", "Clock" };
    for (int r = 0; r < 3; ++r)
    {
        g.setColour (bright);
        g.setFont (font (13.0f));
        g.drawText (names[r], 524, kRowY[r] - 8, 64, 16, juce::Justification::centredLeft);
        g.setColour (dim);
        g.setFont (font (11.0f));
        juce::String number;
        if (r < 2) number = "CC " + juce::String (particlehold::kCoarseCc[r]) + " + " + juce::String (particlehold::kFineCc[r]);
        else if (mode == particlehold::sendAsNote) number = "note C2";
        else if (mode == particlehold::sendAsBoth) number = "CC 79 + C2";
        else                                       number = "CC 79";
        g.drawText (number, 588, kRowY[r] - 8, 90, 16, juce::Justification::centredLeft);
    }
    g.setColour (dim);
    g.setFont (font (12.0f));
    g.drawText ("send as", 480, 347, 60, 16, juce::Justification::centredLeft);

    g.setColour (faint);
    g.setFont (font (11.0f));
    g.drawText (JucePlugin_VersionString, 700, 446, 60, 14, juce::Justification::centredRight);
}

// ================================================================================================ editor
ParticleholdEditor::ParticleholdEditor (ParticleholdProcessor& p)
    : AudioProcessorEditor (&p), panel (p), proc (p)
{
    addAndMakeVisible (panel);
    constrainer.setFixedAspectRatio ((double) kWidth / (double) kHeight);
    constrainer.setSizeLimits (kMinWidth, heightForWidth (kMinWidth), kMaxWidth, heightForWidth (kMaxWidth));
    setConstrainer (&constrainer);
    setResizable (true, true);                       // with the drag corner

    const int saved = (int) proc.getValueTreeState().state.getProperty (widthProperty, kWidth);
    const int width = juce::jlimit (kMinWidth, kMaxWidth, saved);
    setSize (width, heightForWidth (width));
}

ParticleholdEditor::~ParticleholdEditor() = default;

void ParticleholdEditor::paint (juce::Graphics& g) { g.fillAll (PhStyle::panel); }

void ParticleholdEditor::resized()
{
    const double scale = juce::jmin ((double) getWidth() / kWidth, (double) getHeight() / kHeight);
    panel.setTransform (juce::AffineTransform::scale ((float) scale));
    panel.setBounds (0, 0, kWidth, kHeight);

    auto& state = proc.getValueTreeState().state;
    if ((int) state.getProperty (widthProperty, -1) != getWidth())
        state.setProperty (widthProperty, getWidth(), nullptr);
}
