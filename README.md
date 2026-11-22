# particlehold

An original AU MIDI effect for Logic Pro: a sample and hold. Notes (or its own clock) in; the held value, the white
noise and the clock out as MIDI CC, which a Scripter mapper moves onto any knob on the same track. Inspired by the
Instruō tágh (legacy version), a sample and hold module. It is not a copy, contains no Instruō code or artwork, and is
not affiliated with Instruō. The tágh is an analog module with no published firmware; its manual was used only to
understand what the controls do. The maths is our own. It follows Bryn and reuses its engine.

Status: **version 0.1.0, milestone 1 (the only milestone) built; waiting to be tested in Logic on both Macs.**

## What it does

Put particlehold in a **MIDI FX slot** above any instrument. Your notes sound as normal; each new note also takes a
new value and holds it, the panel shows it, and it goes out as MIDI CC. Defaults: Sample and hold, Clock Notes,
Source Noise, Level 100%, rectifier off, Slew off, MIDI out channel 13, Signal and Clock on, Noise off, Send as CC.

- **Mode** (the module's toggle):
  - *Sample and hold*: a new value at every rising edge of the clock (a new note, an internal clock tick, the
    Trigger button), held until the next.
  - *Hold and track*: follows the input while the clock is high (a note held, the first half of each internal tick,
    the button held), and holds the value it had when the clock fell.
- **Source** (the module's Signal input; with nothing plugged in, the module samples its own white noise):
  - *Noise* (the default): white noise, every value from the bottom to the top equally likely.
  - *CC*: the controller chosen in the **Source CC** menu (1, the mod wheel, by default), from any channel. A
    controller's middle (64) counts as 0 V. If the number is the first half of a 14-bit pair (CC 0-31 with 32-63, or
    the particledots pairs: Bryn's 80 / 81, Strange Drift's 106-112 / 113-119), the second half is read too, so a
    plugin sitting above particlehold is sampled at full detail.
  - *Aftertouch* (channel or polyphonic, the latest of either), *Pitch bend*, *Velocity* (of the latest note).
- **Level** (0 to 100%): scales the input towards 0 V. At 100% with the rectifier off a sampled controller comes out
  at exactly the value it went in.
- **Rectifier** (off / on): flips everything below 0 V upward, so the output only goes up from 0 V.
- **0 V**: with the rectifier off, 0 V is the middle of the output (8192 of 16,383): values go both ways from it.
  With the rectifier on, 0 V is the bottom (0). Level and the rectifier work on the input, as on the module: they
  change the next value, not the one being held.
- **Slew** (Off, or 5 ms to 10 s; the knob's far left is Off, its centre 224 ms): each new value is reached by a glide
  that starts fast and slows as it arrives, like the module's analog slew. A big jump and a small one take the same
  time; the glide arrives exactly Slew after the sample moment. A new value during a glide starts from where the output
  is: no jump. In hold and track the output chases the moving input smoothly (the manual's "inverse low pass filter").
- **Clock** (the module's normalled internal clock and its Clk In):
  - *Notes* (the default): your notes are the clock.
  - *Internal*: **Rate** and **Range**, Low 0.05 to 4 Hz (one step every 20 s up to 4 a second), High 0.5 to 40 Hz;
    equal ratios for equal turns, centre 0.45 Hz Low, 4.5 Hz High. High for the first half of each tick.
  - *Button only* (the module's "dummy cable"): only the Trigger button.
  - The round **Trigger button** works with all three (it and the clock act as one gate: pressed while the clock is
    already high it adds nothing). It can be automated; a project never opens with it held.
- **Outputs**, each with its own switch:
  - *Signal*: the held value, after the slew, as 14-bit CC. Switched off it sends 0 V once, then nothing.
  - *Noise* (off by default): the white noise as 14-bit CC, a new value about every 6 ms (as fast as a CC usefully
    goes); about 330 MIDI messages a second, hence off by default.
  - *Clock*: high while the clock is high, at least 20 ms (back-to-back notes merge into one longer gate).
    **Send as**: *CC* (127 while high, then 0; for the mapper), *Note* (C2, velocity 100, released when it falls;
    plays the instrument, for example a drum pad) or *Both*, on the MIDI out channel.
- **The panel** shows, live: the last 4 seconds (the input in grey, the output in white above 0 V and amber below, a dot
  at each sample moment); the present value as the module's LED (white above 0 V, amber below, brighter further out)
  with the number and *holding / tracking / gliding*; the clock and track lights; each output's level bar and light.
- **Resizing**: drag the bottom-right corner; 80% to 200% (624 x 376 to 1560 x 940), proportions locked, size saved
  with the project.

## Notes and the clock

The same rules as Bryn (its `Keys` is used unchanged):

- With Clock on Notes, the clock is high while **any** key is held (any channel), or while the Trigger button is held.
- **A chord** is one sample moment. **Overlapping (legato) notes**: a new key while another is held is not a new
  sample moment. **Back-to-back notes** (one ends at the exact sample the next starts) are two.
- A key pressed twice without a release counts once; its first release lets it go (a missing note-off can never hold
  the clock high). Velocity 0 is a release. All Notes Off / All Sound Off release that channel's keys.
- A zero-length note (on and off at the same sample) is a one-sample clock: one sample moment.
- **Rest**: transport stop, Logic resetting the plugin, bypass, and a pause of more than a second in Logic's calls send
  Signal and Noise straight to 0 V (sent if not already there), end the Clock (CC 0 and/or note-off), release every
  key and forget the Trigger button until it is let go. A key's later release then does nothing. The internal clock
  starts again: its next tick comes half a tick later, so 0 V is always sent first. The Noise output runs on whenever
  Logic runs the plugin.
- **A sample and hold holds**: after the last note the Clock goes low but Signal keeps its value (that is the point of
  it) until the next note or a rest.
- **Changing a control never makes a sample moment or starts the Clock.** A new Clock source is taken as it stands:
  if it is high (a key already held), it counts once it has been low. Switching to hold and track while the clock is
  high waits for the next edge; switching back to sample and hold holds where it is. Changing Send as or the channel,
  or switching an output off, ends whatever it had on at once; changing the channel sends 0 V on the old channel.
- Zero latency: every incoming message (notes, controllers, pitch bend, aftertouch, program changes, sysex) passes on
  unchanged, at its own sample, in order. One limit: a plugin cannot send MIDI while it is being removed from its slot.

## How the values reach a knob in Logic

Logic has no CV. Signal and Noise go out as 14-bit CC pairs (16,384 steps instead of 128), the Clock as a CC (127 / 0),
a note, or both, all on channel 13:

| Output | Signal | Noise | Clock |
|---|---|---|---|
| coarse CC | 75 | 77 | 79 |
| fine CC | 76 | 78 | |
| note (Note / Both) | | | 48 (C2) |
| from | 0.1.0 | 0.1.0 | 0.1.0 |

None of these numbers is used by Bryn (channel 14: CC 80-83, 90, notes 45-47), Hillside (channel 16: CC 3, 35, 9, 41,
85-89, notes 40-44), Ogdoad (channel 16: CC 12-31, 44-63, 102-105, notes 36-39) or Strange Drift (channel 15:
CC 106-119), on any channel, so all five can share a track (even all on one channel). Every CC that MIDI leaves
completely without a job is taken by those four; CC 75-79 are the next best: MIDI names them "sound controllers 6-10"
and only suggests uses for 75-78 (decay, vibrato rate, depth, delay); 79 has none. The mapper stops them before the
instrument. The pairs are direct pairs (75 / 76, 77 / 78), as Bryn's 80 / 81.

The **particlehold Mapper** (a Scripter script, in the download as `particlehold Mapper (paste into Scripter).txt`,
source in `tools/mapper/particlehold-Mapper.js`) sits in a MIDI FX slot below particlehold. It has 8 lanes; each lane
has a Source (Off, Signal, Noise, Clock), a Target (Logic's *Learn plug-in parameter*), and Min % / Max %. It moves the
learnt knob on any plugin below it on the same channel strip and stops particlehold's CC from reaching the instrument;
everything else passes through, the other four plugins' CC included. The Clock source jumps the knob from Min to Max
while high (Send as CC or Both). With the rectifier off, 0 V sits in the middle of Min..Max; with it on, at Min.
`INSTALL.txt` in the download has the steps.

**With Bryn, Hillside, Ogdoad and Strange Drift on the same track:** the plugins first, then the mappers in any order.
particlehold only hears what comes from above it, so to sample another plugin's output put particlehold below it and
choose that CC as the Source (CC 20 is Ogdoad's LFO 1: the manual's "Track & Hold Synth Voice" patch). Trigger notes
from a plugin above (Ogdoad's, or Bryn's gates with Send as Note) clock it too (the manual's "West Coast Synth Voice").

**Smoothness and CPU.** As Bryn (its `CcStream` rules): a value is sent once it has moved at least 0.2% and 6 ms have
passed, or after 100 ms for smaller moves, and at most once per audio block; nothing while it stands still. Every new
value (without the slew) and every glide's arrival is exact: sent at its own sample. That is why the fastest clock is
40 Hz: every step still reaches Logic at any buffer up to 1024 (checked at 44.1 kHz). At Logic's usual 256-sample
buffer, glides of about **50 ms or longer** move a knob smoothly. Measured in the engine test (buffer 256): a 4 Hz
clock with a 100 ms slew sends about 110 MIDI messages a second (Signal and Clock); the Noise output alone about 330;
hold and track on noise about 170. The engine uses under 0.1% of one core at its heaviest settings.

## The manual, and how it was read

1. **Signal input** (no CV in Logic): the Source menu: the noise (as on the module with nothing plugged in), any
   controller (14-bit where it is a pair), aftertouch, pitch bend or velocity.
2. **White noise output**: raw noise changes every sample and cannot travel as MIDI CC. The noise stays inside, as the
   default Source; its output jack becomes the optional Noise output, a new value at every MIDI step (about 6 ms).
3. **Clk In**: notes (Clock: Notes). The dummy cable: Clock: Button only.
4. **Clock speeds**: not given. Low 0.05-4 Hz, High 0.5-40 Hz (MIDI cannot carry the module's audio rates).
5. **Internal clock high time**: not given; hold and track needs one. Half of each tick.
6. **Noise**: its spread and loudness are not given. Evenly spread, full scale at Level 100%.
7. **Slew**: times and curve not given. Off to 10 s, a fast-then-slowing glide taking the same time for any jump (an
   analog slew through a capacitor), which fits the manual calling it an inverse low-pass control.
8. **Hold and track** "locks onto a steady voltage derived from the input": the input's value at the last sample the
   clock was high.
9. **Manual Trigger with a clock running**: the button and the clock are one gate.
10. **Level before or after the rectifier**: not said; the result is the same either way.
11. **Bipolar and unipolar**: the output's 0 V is the middle (rectifier off) or the bottom (on); the mapper's Min and Max
    place it on the knob.
12. **The LED** (white positive, amber negative, brightness = amplitude): the panel's big light, the scope's colours
    and the Signal light.

## Left out

- The audio patches (audio-rate downsampling, the slew as a low-pass filter on audio), audio-rate clocks, the noise as
  audio: MIDI carries none of these.
- CV into the Signal input beyond the MIDI sources above.
- The tágh V2.

## Later (not planned; only built if asked)

- Clock synced to the project tempo.
- Every note-on as a new sample moment, even legato.
- Separate up and down slew.
- Bell-shaped noise (crowding near the middle).
- A "positive / negative" gate output.

## Folders

| Folder | What it is |
|---|---|
| `engine/` | The engine. Plain C++, no plugin code. From Bryn 0.1.0, unchanged except the namespace: `Keys`, `Curve`. From Bryn with one addition (a rest value per output): `CcStream`. Adapted from Bryn: `Pulses` (the Clock output), `Engine`. New: `Clock` (the internal clock), `Noise`, `Glide` (the slew, on `Curve`), `Outputs.h` (the MIDI numbers). |
| `plugin/` | The plugin wrapper: what Logic sees (an AU MIDI effect), the list of controls (`Parameters`), and the panel (`PluginEditor`, `Style`). |
| `check/` | `particleholdCheck`: a console program that tests the plugin layer without a host; `golden/` holds the control list and a saved state of each version. |
| `tools/mapper/` | The Scripter mapper script. |
| `tools/install/` | `Install-Unofficial-Plugin.command`, the same installer as the other particledots plugins (Audio Unit only). |
| `.github/workflows/` | The automatic build. |

## Checks run on every push

- **Engine self-test** (Linux): the defaults; timing: the same trace sample for sample at every block size (1, 32 to
  1024 and random) in five setting groups, every step and glide arrival at its exact sample in the MIDI; internal
  clock ticks within one sample of (k + 0.5) / rate at 44.1, 48 and 96 kHz from 0.15 to 40 Hz; slews of 5 ms to 3 s
  arriving within one sample at all three rates; notes at their own samples at all three rates; sample and hold and
  hold and track; Level and the rectifier and the 0 V point; every source (14-bit pairs of every particledots plugin,
  aftertouch, pitch bend, velocity); the slew's shape, same time for any jump, no jump mid-glide or on a new Slew;
  Rate and Range; Internal / Notes / Button only and the Trigger button; rate changes keeping the phase; the note rules
  (chords, legato, back-to-back, zero-length, double presses, All Notes Off); rest (0 V and the Clock's end at the
  first sample, the later release ignored, bypass); 200 control changes while a key is held starting nothing; outputs
  switched off and channel changes; every step reaching the receiver at 40 Hz with a 1024 buffer; the 6 ms / 0.2%
  rule; the noise evenly spread; two stuck-note fuzzes of about 50,000 random events each; CPU and MIDI rate.
- **Mapper test**: the real mapper script in a stand-in for Scripter, fed by the engine's real MIDI stream: Signal and
  Noise lanes value for value, Min / Max, upside down, 14-bit detail, the Clock source following the real Clock stream
  (and the Clock note passing to the instrument in Note and Both), Off lanes, the channel setting, notes and everyone
  else's CC passing, and the five mappers (particlehold, Bryn, Hillside, Ogdoad, Strange Drift) chained in all 120
  orders, with particlehold on its own channel and on Hillside's and Ogdoad's.
- **Plugin-layer check** (`particleholdCheck`): golden control list and saved state; MIDI-effect flags, no audio buses,
  no latency, the defaults; the MIDI path (everything passes unchanged and in order, channel 13, 0 V at the start, a new
  value and the Clock at the note's sample, velocity 0, 0- and 1-sample blocks, another out channel, Note and Both, the
  internal clock without notes, every source read from real MIDI); automation of every control with and without a held
  note; transport stop, reset, releaseResources, prepareToPlay, All Notes Off, bypass and a pause in Logic's calls while
  tracking, with the Clock as CC and as notes; the internal clock after a stop; two stuck-note fuzzes through the real
  plugin (one with bypass); the panel (design size, every control live and moving only its own parameter, the texts
  under the knobs, the Trigger button, the clock and track lights, the output lights, the scope, resizing limits, saved
  size). The run uploads pictures of the window (artifact **particlehold-window-pictures**).
- **macOS build**: universal binary (Apple Silicon + Intel), ad-hoc signing, Apple's `auval -v aumi Phld Pdot`,
  pluginval at strictness 5 (AU and VST3), and the package (installer identical to the shared one, INSTALL.txt with
  the mapper steps, the mapper).

## Getting the plugin

1. Open the **Actions** tab of this repo and click the latest **Build particlehold** run.
2. At the bottom, under **Artifacts**, download **particlehold-macOS** and unzip it (twice: the download is a zip
   containing `particlehold-macOS.zip`).
3. Inside the `particlehold-macOS` folder are `particlehold.component`, `particlehold.vst3`, `INSTALL.txt`,
   `auval.log`, the pluginval logs, `particlehold Mapper (paste into Scripter).txt` and
   `Install-Unofficial-Plugin.command`.
4. In Terminal, once, clear the download block: `xattr -dr com.apple.quarantine ~/Downloads/particlehold-macOS/*`
   (adjust the path if you unzipped elsewhere).
5. **Easy install (Logic, Audio Unit):** double-click `Install-Unofficial-Plugin.command`. It asks for your Mac
   password once, installs the AU for all users and restarts the AU registrar.
6. In Logic it is a MIDI effect: MIDI FX slot of a Software Instrument track, **Audio Units > particledots >
   particlehold**.

## Fixed names

Plugin code `Phld`, manufacturer `Pdot`, bundle id `com.particledots.particlehold`, AU type MIDI effect (`aumi`),
target `particlehold`, engine library `particlehold_engine`. Parameter ids, in order: `mode`, `level`, `rectify`,
`slew`, `rate`, `range`, `clock`, `source`, `sourceCc`, `trigger`, `outChannel`, `signalOn`, `noiseOn`, `clockOn`,
`sendAs`. Saved with the project besides the parameters: `editorWidth`. The CC and note numbers above and the mapper's
Source menu order are fixed too. These never change, so saved projects keep finding the plugin and its settings; new
controls can only be added at the end.
