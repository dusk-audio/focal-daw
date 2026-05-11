#pragma once

#include <juce_core/juce_core.h>
#include "Session.h"

namespace focal::snap
{
    // Beat step in samples when bpm > 0. Returns 0 otherwise. Use this
    // when callers want to *skip* snapping entirely if there's no tempo
    // (e.g. snap-to-bar/beat for a brand-new region that has no prior
    // origin to drift from).
    inline juce::int64 beatSamples (double sampleRate, float bpm) noexcept
    {
        if (sampleRate <= 0.0 || bpm <= 0.0f) return 0;
        return (juce::int64) (sampleRate * 60.0 / (double) bpm);
    }

    // Beat step in samples when bpm > 0, otherwise 1 second. Returns 0
    // only when the sample rate itself is invalid. Use this for delta
    // drags where the user expects *some* snapping even without a
    // tempo (the existing TapeStrip drag/marker pattern).
    inline juce::int64 beatOrSecondSamples (double sampleRate, float bpm) noexcept
    {
        if (sampleRate <= 0.0) return 0;
        return (bpm > 0.0f)
            ? (juce::int64) (sampleRate * 60.0 / (double) bpm)
            : (juce::int64) sampleRate;
    }

    // Round a signed delta to the nearest multiple of `step`. Returns the
    // input unchanged when step <= 0. The half-step bias is sign-aware so
    // the delta is rounded toward zero by less than half a step in either
    // direction.
    inline juce::int64 snapDelta (juce::int64 delta, juce::int64 step) noexcept
    {
        if (step <= 0) return delta;
        return ((delta + (delta >= 0 ? step / 2 : -step / 2)) / step) * step;
    }

    // Round a non-negative absolute sample position to the nearest multiple
    // of `step`. Negative input or step <= 0 returns the input unchanged.
    inline juce::int64 snapAbsolute (juce::int64 sample, juce::int64 step) noexcept
    {
        if (step <= 0 || sample < 0) return sample;
        return ((sample + step / 2) / step) * step;
    }

    // Convenience: gate the delta-snap on session.snapToGrid using the
    // beat-or-second step model. Matches the four sites in TapeStrip.cpp.
    inline juce::int64 snapDeltaToGrid (juce::int64 delta, const Session& s,
                                        double sampleRate) noexcept
    {
        if (! s.snapToGrid) return delta;
        return snapDelta (delta, beatOrSecondSamples (sampleRate,
                                                     s.tempoBpm.load (std::memory_order_relaxed)));
    }

    // Convenience: gate the absolute-snap on session.snapToGrid using the
    // beat-only step model. Matches the new-MIDI-region site in TapeStrip.cpp.
    inline juce::int64 snapAbsoluteToGrid (juce::int64 sample, const Session& s,
                                           double sampleRate) noexcept
    {
        if (! s.snapToGrid) return sample;
        return snapAbsolute (sample, beatSamples (sampleRate,
                                                 s.tempoBpm.load (std::memory_order_relaxed)));
    }
}
