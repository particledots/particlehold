// From Bryn 0.1.0 (itself from Hillside 0.2.0); the only change: a lane's rest value replaces the fixed 0 (see CcStream.h).
#include "particlehold/CcStream.h"

#include <cmath>

namespace particlehold
{
    void CcStream::prepare (double sampleRate)
    {
        minInterval = (long long) std::ceil (kMinIntervalSeconds * sampleRate - 1.0e-9);
        quiet       = (long long) std::ceil (kQuietSeconds * sampleRate - 1.0e-9);
        forget();
    }

    void CcStream::setLane (int lane, int coarseCc, int fineCc)
    {
        auto& l = lanes[(std::size_t) lane];
        l.coarseCc = coarseCc;
        l.fineCc = fineCc;
        if (lane + 1 > count) count = lane + 1;
    }

    void CcStream::forget()
    {
        for (auto& l : lanes)
        {
            l.last = -1; l.lastCoarse = -1; l.lastAbs = -(1LL << 40);
            l.has = l.hasCorner = false;
        }
    }

    void CcStream::consider (int lane, long long absSample, int offset, int v, bool corner)
    {
        auto& l = lanes[(std::size_t) lane];
        if (! l.enabled) return;
        if (corner)
        {
            if (v != l.last || l.has)
            {
                l.has = l.hasCorner = true;
                l.candOffset = offset; l.candValue = v; l.candAbs = absSample;
            }
            return;
        }
        if (l.has || v == l.last) return;   // one per block; nothing to send if it stands still
        const long long since = absSample - l.lastAbs;
        if (since < minInterval) return;
        const int moved = l.last < 0 ? 16384 : (v > l.last ? v - l.last : l.last - v);
        if (moved < kMinStep && since < quiet) return;
        l.has = true;
        l.candOffset = offset; l.candValue = v; l.candAbs = absSample;
    }

    void CcStream::force (int lane, long long absSample, int offset, int v)
    {
        auto& l = lanes[(std::size_t) lane];
        if (! l.enabled) return;
        if (v == l.last && ! l.has) return;
        l.has = l.hasCorner = true;
        l.candOffset = offset; l.candValue = v; l.candAbs = absSample;
    }

    void CcStream::setEnabled (int lane, bool on, long long absSample, int offset)
    {
        auto& l = lanes[(std::size_t) lane];
        if (on == l.enabled) return;
        if (! on)
        {
            if (l.last >= 0 && l.last != l.rest)   // send the rest value once, then nothing
            {
                l.has = l.hasCorner = true;
                l.candOffset = offset; l.candValue = l.rest; l.candAbs = absSample;
            }
            else l.has = l.hasCorner = false;   // already at rest, or never sent anything: stay silent
            l.enabled = false;
        }
        else
        {
            l.enabled = true;
            l.last = -1; l.lastCoarse = -1; l.lastAbs = -(1LL << 40);
            l.has = l.hasCorner = false;
        }
    }

    void CcStream::moveChannel (int oldChannel, int offset, MidiSink& sink)
    {
        for (int i = 0; i < count; ++i)
        {
            auto& l = lanes[(std::size_t) i];
            if (l.last >= 0 && l.last != l.rest)
            {
                if (l.lastCoarse != (l.rest >> 7)) sink.cc (offset, oldChannel, l.coarseCc, l.rest >> 7);
                sink.cc (offset, oldChannel, l.fineCc, l.rest & 127);
            }
            l.last = -1; l.lastCoarse = -1; l.lastAbs = -(1LL << 40);
            l.has = l.hasCorner = false;
        }
    }

    void CcStream::flush (int channel, MidiSink& sink)
    {
        // At most one message pair per lane; send in sample order (lanes in order at the same sample).
        int order[kMaxLanes];
        int n = 0;
        for (int i = 0; i < count; ++i)
            if (lanes[(std::size_t) i].has) order[n++] = i;
        for (int a = 1; a < n; ++a)
            for (int b = a; b > 0 && lanes[(std::size_t) order[b]].candOffset < lanes[(std::size_t) order[b - 1]].candOffset; --b)
            { const int t = order[b]; order[b] = order[b - 1]; order[b - 1] = t; }

        for (int k = 0; k < n; ++k)
        {
            auto& l = lanes[(std::size_t) order[k]];
            const int v = l.candValue, coarse = v >> 7, fine = v & 127;
            if (coarse != l.lastCoarse)
            {
                sink.cc (l.candOffset, channel, l.coarseCc, coarse);
                l.lastCoarse = coarse;
            }
            sink.cc (l.candOffset, channel, l.fineCc, fine);
            l.last = v;
            l.lastAbs = l.candAbs;
            l.has = l.hasCorner = false;
        }
    }
}
