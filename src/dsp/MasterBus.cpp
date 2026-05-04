#include "MasterBus.h"

namespace adhdaw
{
MasterBus::MasterBus()
    : oversampler (2 /*channels*/, 2 /*stages = 4x*/,
                   juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
                   /*isMaximumQuality*/ true)
{
}

void MasterBus::prepare (double sampleRate, int blockSize)
{
    faderGain.reset (sampleRate, 0.020);
    faderGain.setCurrentAndTargetValue (1.0f);

#if ADHDAW_HAS_DUSK_DSP
    tape.prepare (sampleRate, juce::jmax (1, blockSize));
    tape.setDrive (kTapeDrive);
    tape.reset();
#endif

    preparedBlockSize = blockSize;
    oversampler.initProcessing ((size_t) juce::jmax (1, blockSize));
    oversampler.reset();
    oversamplerReady = true;
}

void MasterBus::processInPlace (float* L, float* R, int numSamples) noexcept
{
    const bool tapeOn = paramsRef != nullptr
                       && paramsRef->tapeEnabled.load (std::memory_order_relaxed);
    const bool hq     = paramsRef != nullptr
                       && paramsRef->tapeHQ.load (std::memory_order_relaxed);

    if (paramsRef != nullptr)
    {
        const float faderDb = paramsRef->faderDb.load (std::memory_order_relaxed);
        const float gain = (faderDb <= ChannelStripParams::kFaderInfThreshDb)
                           ? 0.0f
                           : juce::Decibels::decibelsToGain (faderDb);
        faderGain.setTargetValue (gain);
    }

#if ADHDAW_HAS_DUSK_DSP
    if (tapeOn && hq && oversamplerReady)
    {
        // 4× path: upsample, run the stereo tape process at the upsampled rate
        // (so L/R IIR filter states stay independent — processSampleMono uses
        // the L-channel filters only and would corrupt state if alternated
        // between channels), then downsample.
        float* channels[2] = { L, R };
        juce::dsp::AudioBlock<float> block (channels, 2, (size_t) numSamples);
        auto upBlock = oversampler.processSamplesUp (block);

        const auto upSamples = (int) upBlock.getNumSamples();
        float* upChannels[2] = { upBlock.getChannelPointer (0),
                                  upBlock.getChannelPointer (1) };
        juce::AudioBuffer<float> upBuf (upChannels, 2, upSamples);
        tape.process (upBuf);
        oversampler.processSamplesDown (block);
    }
    else if (tapeOn)
    {
        // 1× path: stereo block process at native rate (handles drive smoothing).
        float* channels[2] = { L, R };
        juce::AudioBuffer<float> tapeBuf (channels, 2, numSamples);
        tape.process (tapeBuf);
    }
#endif

    for (int i = 0; i < numSamples; ++i)
    {
        const float g = faderGain.getNextValue();
        L[i] *= g;
        R[i] *= g;
    }
}
} // namespace adhdaw
