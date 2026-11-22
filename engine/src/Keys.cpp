// From Bryn 0.1.0 (itself from Hillside 0.2.0), unchanged except the namespace (bryn -> particlehold).
#include "particlehold/Keys.h"

namespace particlehold
{
    namespace
    {
        bool valid (int channel, int note) { return channel >= 0 && channel < 16 && note >= 0 && note < 128; }
    }

    void Keys::beginSample (long long sampleIndex)
    {
        now = sampleIndex;
        heldAtStart = count;
        releasedOld = 0;
        pressedNew = 0;
        blips = 0;
    }

    void Keys::press (int channel, int note)
    {
        if (! valid (channel, note)) return;
        if (down[channel][note])
        {
            if (pressedAt[channel][note] != now) { repressedAt[channel][note] = now; ++pressedNew; }
            return;
        }
        down[channel][note] = true;
        pressedAt[channel][note] = now;
        ++count;
        ++pressedNew;
    }

    void Keys::release (int channel, int note)
    {
        if (! valid (channel, note) || ! down[channel][note]) return;
        if (repressedAt[channel][note] == now && pressedAt[channel][note] != now)
        {   // the old note of a key pressed again at this sample: the key stays held (back-to-back repeat)
            repressedAt[channel][note] = -1;
            pressedAt[channel][note] = now;
            ++releasedOld;
            return;
        }
        down[channel][note] = false;
        --count;
        if (pressedAt[channel][note] != now) ++releasedOld;
        else { --pressedNew; ++blips; }   // pressed and released at the same sample: a zero-length note
    }

    void Keys::releaseChannel (int channel)
    {
        if (channel < 0 || channel >= 16) return;
        for (int n = 0; n < 128; ++n) { repressedAt[channel][n] = -1; release (channel, n); }
    }

    void Keys::clear()
    {
        for (auto& ch : down) for (auto& d : ch) d = false;
        for (auto& ch : repressedAt) for (auto& r : ch) r = -1;
        count = 0;
        heldAtStart = releasedOld = pressedNew = blips = 0;
    }
}
