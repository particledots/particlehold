// particlehold's MIDI numbers. Fixed in the first build (0.1.0) and never changed: the mapper and saved projects rely
// on them.
//
// Chosen so particlehold can share one track with Bryn (channel 14: CC 80-83, 90, notes 45-47), Hillside (channel 16:
// CC 3, 35, 9, 41, 85-89, notes 40-44), Ogdoad (channel 16: CC 12-31, 44-63, 102-105, notes 36-39) and Strange Drift
// (channel 15: CC 106-119): none of particlehold's numbers is used by any of them, on any channel, and particlehold
// sends on its own channel (13) by default.
// Every CC that MIDI leaves completely without a job is taken by those four. CC 75-79 are the next best: MIDI names
// them "sound controllers 6-10" and only suggests uses for 75-78 (decay, vibrato rate, depth, delay); 79 has none.
// The mapper stops them before the instrument. The 14-bit pairs are direct pairs (75 / 76, 77 / 78), as Bryn's 80 / 81,
// since every coarse CC with a standard fine partner (0-31) is taken. The Clock note follows Bryn's 45-47.

#pragma once

namespace particlehold
{
    enum OutputIndex
    {
        outSignal = 0,   // the held (or tracked) value, after the slew
        outNoise  = 1,   // the white noise, a new value at every MIDI step
        kNumSmoothOutputs = 2
    };

    inline constexpr int kCoarseCc[kNumSmoothOutputs] = { 75, 77 };
    inline constexpr int kFineCc[kNumSmoothOutputs]   = { 76, 78 };

    // The pulse output: Clock (high while the clock that drives the sample and hold is high, at least 20 ms).
    // CC: 127 while high, then 0. Note: velocity 100, same timing.
    inline constexpr int kNumPulseOutputs = 1;
    inline constexpr int kClockIndex = 0;
    inline constexpr int kPulseCc[kNumPulseOutputs]   = { 79 };
    inline constexpr int kPulseNote[kNumPulseOutputs] = { 48 };   // C2
    inline constexpr int kPulseVelocity = 100;
    inline constexpr double kPulseSeconds = 0.020;   // the shortest gate

    // "Send as" (the control's menu order): CC, Note, Both.
    enum SendAs { sendAsCc = 0, sendAsNote = 1, sendAsBoth = 2 };

    inline constexpr int kDefaultChannel = 13;

    // 0 V on the 14-bit scale: the middle. (With the rectifier on, the Signal output's 0 V is the bottom, 0.)
    inline constexpr int kMiddle14 = 8192;
}
