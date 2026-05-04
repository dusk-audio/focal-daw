#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "../session/Session.h"

#if ADHDAW_HAS_DUSK_DSP
  #include "TapeSaturation.h"   // tape-echo/Source/DSP — TapeEchoDSP::TapeSaturation
#endif

namespace adhdaw
{
// Phase 1a master bus: tape saturation (on/off + HQ) + master fader. Pultec
// EQ + bus compressor will insert before the tape stage in later chunks.
class MasterBus
{
public:
    MasterBus();

    void prepare (double sampleRate, int blockSize);
    void bind (const MasterBusParams& params) noexcept { paramsRef = &params; }

    void processInPlace (float* L, float* R, int numSamples) noexcept;

private:
    const MasterBusParams* paramsRef = nullptr;
    juce::SmoothedValue<float> faderGain { 1.0f };

    // Tape drive on tape-echo's TapeSaturation is a normalised [0..1] amount
    // (smoothed internally). 0.5 = moderate saturation, matching the prior
    // `kTapeDrive=4.0f` operating point on the old dsp-cores model. Bias is
    // not exposed by tape-echo's class.
    static constexpr float kTapeDrive = 0.5f;

#if ADHDAW_HAS_DUSK_DSP
    TapeEchoDSP::TapeSaturation tape;  // stereo — handles both channels
#endif

    // 4x oversampler used when HQ is engaged. 2 stages of 2x each = 4x.
    juce::dsp::Oversampling<float> oversampler;
    int  preparedBlockSize = 0;
    bool oversamplerReady = false;
};
} // namespace adhdaw
