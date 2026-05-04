#pragma once

#include <juce_core/juce_core.h>
#include <atomic>

namespace adhdaw
{
// Phase 2 minimum: Stopped / Playing / Recording. Punch-in/out + Loop arrive
// in Phase 3 per the spec. The state itself is a single atomic so the audio
// thread can read it lock-free; transitions happen on the message thread.
class Transport
{
public:
    enum class State : int { Stopped = 0, Playing = 1, Recording = 2 };

    State getState() const noexcept { return state.load (std::memory_order_relaxed); }
    bool isStopped()   const noexcept { return getState() == State::Stopped; }
    bool isPlaying()   const noexcept { return getState() == State::Playing; }
    bool isRecording() const noexcept { return getState() == State::Recording; }

    void setState (State s) noexcept { state.store (s, std::memory_order_relaxed); }

    juce::int64 getPlayhead() const noexcept { return playheadSamples.load (std::memory_order_relaxed); }
    void setPlayhead (juce::int64 s) noexcept { playheadSamples.store (s, std::memory_order_relaxed); }

    // Called from the audio callback when state is Playing or Recording.
    void advancePlayhead (int numSamples) noexcept
    {
        playheadSamples.fetch_add (numSamples, std::memory_order_relaxed);
    }

private:
    std::atomic<State>       state            { State::Stopped };
    std::atomic<juce::int64> playheadSamples  { 0 };
};
} // namespace adhdaw
