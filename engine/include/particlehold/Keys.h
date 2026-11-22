// From Bryn 0.1.0 (itself from Hillside 0.2.0), unchanged except the namespace (bryn -> particlehold).
// Which keys are held, on all 16 channels: the gate from notes. Plain C++, no plugin code.
//
// Rules (shown in the README):
//   - The gate is high while any key is held (or, in the Engine, while the panel's Gate button is held).
//   - A new key while another is held does not restart the envelope (a held gate has no new edge on the module).
//   - Releasing a key while another is still held does nothing; the gate falls when the last key is released.
//   - A key pressed again without a release counts once; its first release lets it go (so a missing second note-off
//     can never leave the gate stuck high). Exception: pressed again and released at the same sample (in either
//     order) is a back-to-back repeat of that key: it stays held and the envelope restarts.
//   - Velocity 0 is a release. All Notes Off / All Sound Off (CC 123 / 120) release every key on that channel.
//   - A zero-length note (pressed and released at the same sample) is a one-sample gate when nothing else is held.
//   - Back-to-back: if every key held before a sample is released at that sample and a new key is pressed at the same
//     sample, in either order, the envelope restarts there.

#pragma once

#include <cstdint>

namespace particlehold
{
    class Keys
    {
    public:
        // Call once per sample that has key events, before them.
        void beginSample (long long sampleIndex);
        void press (int channel /*0..15*/, int note);
        void release (int channel, int note);
        void releaseChannel (int channel);
        void clear();

        int held() const { return count; }

        // After the events of a sample: did they make a back-to-back change (all old keys let go, a new one pressed)?
        bool backToBack() const { return heldAtStart > 0 && releasedOld == heldAtStart && pressedNew > 0 && count > 0; }

        // Keys pressed and released at this same sample (a zero-length note). The Engine treats one as a one-sample
        // gate when nothing else is held, so a zero-length note still starts the envelope (in Trig mode: a full attack).
        bool blip() const { return blips > 0; }

    private:
        bool down[16][128] {};
        long long pressedAt[16][128] {};
        long long repressedAt[16][128] {};   // pressed again while down, at this sample
        int count = 0;
        long long now = -1;
        int heldAtStart = 0, releasedOld = 0, pressedNew = 0, blips = 0;
    };
}
