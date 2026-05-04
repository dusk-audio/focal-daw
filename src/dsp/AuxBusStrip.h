#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "../session/Session.h"

namespace adhdaw
{
// Phase 1a placeholder: aux bus = fader + (eventually) bus EQ + bus comp +
// (Phase 1b) plugin slot. Right now we just gate by SIP and apply the fader.
class AuxBusStrip
{
public:
    void prepare (double sampleRate, int blockSize);
    void bind (const AuxBusParams& params) noexcept { paramsRef = &params; }

    // Applies fader (smoothed) to L/R in place; caller has already applied
    // the SIP gate. EQ + bus compressor will insert before the fader stage.
    void processInPlace (float* L, float* R, int numSamples) noexcept;

private:
    const AuxBusParams* paramsRef = nullptr;
    juce::SmoothedValue<float> faderGain { 1.0f };
};
} // namespace adhdaw
