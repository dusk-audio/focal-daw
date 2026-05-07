#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Transport.h"

namespace focal
{
// Minimal AudioPlayHead that reports Focal's transport state to hosted
// plugins. Some donors (TapeMachine in particular) only animate their
// transport-driven UI — reels, VU integration — when the host installs a
// playhead and getCurrentPosition()/getPosition() returns isPlaying.
//
// We only fill the fields the donor cares about (isPlaying, isRecording).
// The sample-position / BPM fields are left at their defaults; if a future
// donor needs them we can add timeInSamples = transport.getPlayhead() etc.
class FocalPlayHead final : public juce::AudioPlayHead
{
public:
    explicit FocalPlayHead (Transport& t) noexcept : transport (t) {}

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying  (transport.isPlaying());
        info.setIsRecording (transport.isRecording());
        return info;
    }

private:
    Transport& transport;
};
} // namespace focal
