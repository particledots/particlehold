# Changelog

## 0.1.0 (finished, 2026-11-22)

- Tested in Logic on both Macs (Apple Silicon M4 and Intel), all 9 steps worked as described: install, notes and
  chords taking new values with the scope and the LED, the mapper moving a filter cutoff, the slew gliding, hold and
  track wandering while held and freezing on release, the internal clock stepping on its own, Send as Note (C2) with
  nothing left sounding after Stop, the settings, mapper lane and window size restored with a saved project, and
  Logic's CPU meter on the Intel Mac with the Noise output and a second mapper lane running. One milestone was enough.
- Automatic checks (GitHub run 45070522103): engine test 248 checks, mapper test 1,975, plugin-layer check 264, none
  failed; auval passed as a MIDI effect; pluginval passed at strictness 5 (AU and VST3); universal binary.


- First build: repo layout, build workflow (engine self-test, mapper test, plugin-layer check with window pictures,
  universal AU + VST3, ad-hoc signing, auval as a MIDI effect, pluginval, package with the shared installer,
  INSTALL.txt and the mapper, private draft release), all from Bryn 0.1.0.
- AU MIDI effect (type `aumi`), no audio buses, zero latency. Codes `Phld` / `Pdot`.
- The engine (plain C++): Bryn's `Keys` and `Curve` unchanged (namespace only); Bryn's `CcStream` with a rest value per
  output; `Pulses` (the Clock output) and `Engine` adapted from Bryn; new `Clock` (internal clock, Low 0.05-4 Hz,
  High 0.5-40 Hz, half-high ticks), `Noise` (evenly spread white noise), `Glide` (the slew: Off, 5 ms to 10 s, the
  same time for any jump, on Bryn's Log curve). Sample and hold, hold and track, Level, the rectifier, the 0 V point
  (middle, or bottom with the rectifier on); Sources Noise, CC (14-bit pairs), Aftertouch, Pitch bend, Velocity;
  Clock Internal, Notes, Button only, with the Trigger button; rest on stop, reset, bypass and pauses.
- Outputs on channel 13 (changeable): Signal as 14-bit CC 75 + 76; Noise as CC 77 + 78 (off by default); Clock as
  CC 79, note 48 (C2) or both (Send as). Each with its own switch.
- The window as approved in the draft: the scope (input and output, 4 s), Source, Source CC, Level, Rectifier, Slew,
  Clock, Rate, Range, Trigger button, Mode switch, the present value with its LED and the clock and track lights,
  outputs with switches, numbers, level bars and lights, Send as. Resizable 80% to 200%.
- Full control list fixed (15 controls). Golden control list and a 0.1.0 saved state in `check/golden/`.
- The particlehold Mapper Scripter script (8 lanes: source, Learn target, min, max; sources Off, Signal, Noise, Clock);
  works alongside the Bryn, Hillside, Ogdoad and Strange Drift mappers.
