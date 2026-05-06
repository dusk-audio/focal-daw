#include "MasterBus.h"
#include <cmath>
#include <cstring>

namespace adhdaw
{
MasterBus::MasterBus() = default;

void MasterBus::bind (const MasterBusParams& params) noexcept
{
    paramsRef = &params;
}

void MasterBus::prepare (double sampleRate, int blockSize, int oversamplingFactor)
{
    faderGain.reset (sampleRate, 0.020);
    faderGain.setCurrentAndTargetValue (1.0f);

    // Clamp to the supported set. Anything else falls back to native rate.
    currentOxFactor = (oversamplingFactor == 2 || oversamplingFactor == 4)
                       ? oversamplingFactor : 1;

#if ADHDAW_HAS_DUSK_DSP
    tape.prepare (sampleRate, juce::jmax (1, blockSize));
    tape.setDrive (kTapeDrive);
    tape.reset();

    tubeEQ.prepare (sampleRate, juce::jmax (1, blockSize), 2);
    tubeEQ.reset();

    busComp.setPlayConfigDetails (2, 2, sampleRate, juce::jmax (1, blockSize));
    busComp.prepareToPlay (sampleRate, juce::jmax (1, blockSize));
    // UC is the standard path here (Bus mode benefits from sidechain HP at
    // 60 Hz et al.) - only its internal oversampling is gated by the global
    // factor. 1× turns the donor's internal 2× off; 2× / 4× both engage the
    // donor's internal 2× (it doesn't expose 4× at the comp level today).
    busComp.setInternalOversamplingEnabled (currentOxFactor > 1);
    compStereoBuffer.setSize (2, juce::jmax (1, blockSize), false, false, true);
    compMidi.clear();
    bindCompParams();
#endif

    preparedBlockSize = blockSize;

    // Tape oversampler - build once per prepare with the right stage count.
    // 1× → no oversampler instance. 2× → 1 stage. 4× → 2 stages.
    const int wantStages = (currentOxFactor == 4) ? 2
                          : (currentOxFactor == 2) ? 1 : 0;
    if (wantStages == 0)
    {
        oversampler.reset();
        oversamplerStages = 0;
    }
    else
    {
        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
            2 /*channels*/,
            (size_t) wantStages,
            juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR,
            /*isMaximumQuality*/ true);
        oversampler->initProcessing ((size_t) juce::jmax (1, blockSize));
        oversampler->reset();
        oversamplerStages = wantStages;
    }
}

#if ADHDAW_HAS_DUSK_DSP
void MasterBus::bindCompParams()
{
    auto& apvts = busComp.getParameters();
    compModeAtom       = apvts.getRawParameterValue ("mode");
    compBypassAtom     = apvts.getRawParameterValue ("bypass");
    compMixAtom        = apvts.getRawParameterValue ("mix");
    compAutoMakeupAtom = apvts.getRawParameterValue ("auto_makeup");
    compBusThreshAtom  = apvts.getRawParameterValue ("bus_threshold");
    compBusRatioAtom   = apvts.getRawParameterValue ("bus_ratio");
    compBusAttackAtom  = apvts.getRawParameterValue ("bus_attack");
    compBusReleaseAtom = apvts.getRawParameterValue ("bus_release");
    compBusMakeupAtom  = apvts.getRawParameterValue ("bus_makeup");
    compBusMixAtom     = apvts.getRawParameterValue ("bus_mix");

    // Lock the comp into Bus mode (CompressorMode::Bus = 3); set wet-only mix.
    storeAtom (compModeAtom, 3.0f);
    storeAtom (compMixAtom,         100.0f);
    storeAtom (compBusMixAtom,      100.0f);
    storeAtom (compAutoMakeupAtom,    0.0f);  // Off
}

void MasterBus::updateEqParameters() noexcept
{
    if (paramsRef == nullptr) return;

    // Value-init padding to zero so the memcmp cache against lastTubeEqParams
    // is reliable - see ChannelStrip equivalent for full reasoning.
    TubeEQProcessor::Parameters p {};
    p.lfBoostGain      = paramsRef->eqLfBoost.load (std::memory_order_relaxed);
    p.lfBoostFreq      = 60.0f;
    p.lfAttenGain      = 0.0f;
    p.hfBoostGain      = paramsRef->eqHfBoost.load (std::memory_order_relaxed);
    p.hfBoostFreq      = 8000.0f;
    p.hfBoostBandwidth = 0.5f;
    p.hfAttenGain      = paramsRef->eqHfAtten.load (std::memory_order_relaxed);
    p.hfAttenFreq      = 10000.0f;
    p.midEnabled       = false;  // Mid Dip/Peak section disabled at master - Pultec users
                                  // typically reach for tube drive instead at the master bus.
    p.midLowFreq = 500.0f;  p.midLowPeak = 0.0f;
    p.midDipFreq = 700.0f;  p.midDip = 0.0f;
    p.midHighFreq = 3000.0f; p.midHighPeak = 0.0f;
    p.inputGain  = 0.0f;
    p.outputGain = paramsRef->eqOutputGainDb.load (std::memory_order_relaxed);
    p.tubeDrive  = paramsRef->eqTubeDrive.load (std::memory_order_relaxed);
    p.bypass     = ! paramsRef->eqEnabled.load (std::memory_order_relaxed);
    if (std::memcmp (&p, &lastTubeEqParams, sizeof (p)) != 0)
    {
        tubeEQ.setParameters (p);
        lastTubeEqParams = p;
    }
}

void MasterBus::updateCompParameters() noexcept
{
    if (paramsRef == nullptr) return;

    storeAtom (compBypassAtom,
               paramsRef->compEnabled.load (std::memory_order_relaxed) ? 0.0f : 1.0f);
    storeAtom (compBusThreshAtom,  paramsRef->compThreshDb.load   (std::memory_order_relaxed));
    storeAtom (compBusRatioAtom,   paramsRef->compRatio.load      (std::memory_order_relaxed));
    storeAtom (compBusAttackAtom,  paramsRef->compAttackMs.load   (std::memory_order_relaxed));
    storeAtom (compBusReleaseAtom, paramsRef->compReleaseMs.load  (std::memory_order_relaxed));
    storeAtom (compBusMakeupAtom,  paramsRef->compMakeupDb.load   (std::memory_order_relaxed));
}
#endif

void MasterBus::processInPlace (float* L, float* R, int numSamples) noexcept
{
    // Contract: numSamples must not exceed what was passed to prepare().
    // If a host violates this, our chunk loop below correctly handles the
    // comp portion; the assert just makes the violation visible in debug.
    jassert (numSamples <= preparedBlockSize);

    const bool tapeOn = paramsRef != nullptr
                       && paramsRef->tapeEnabled.load (std::memory_order_relaxed);
    // Tape oversampling now follows the global factor set at prepare() time.
    // Per-effect tapeHQ is no longer consulted - Session::oversamplingFactor
    // is the single source of truth for "1× / 2× / 4× across all effects".
    const bool useOversampler = (oversampler != nullptr);

    if (paramsRef != nullptr)
    {
        const float faderDb = paramsRef->faderDb.load (std::memory_order_relaxed);
        const float gain = (faderDb <= ChannelStripParams::kFaderInfThreshDb)
                           ? 0.0f
                           : juce::Decibels::decibelsToGain (faderDb);
        faderGain.setTargetValue (gain);
    }

#if ADHDAW_HAS_DUSK_DSP
    // EQ first - tube saturation in TubeEQ feels musical pre-comp.
    // Chunk by preparedBlockSize so a host-driven numSamples > preparedBlockSize
    // never overflows TubeEQ's internal scratch (mirrors the comp chunking
    // below).
    {
        updateEqParameters();
        const int bufSize = juce::jmax (1, preparedBlockSize);
        for (int offset = 0; offset < numSamples; offset += bufSize)
        {
            const int n = juce::jmin (bufSize, numSamples - offset);
            float* channels[2] = { L + offset, R + offset };
            juce::AudioBuffer<float> buf (channels, 2, n);
            tubeEQ.process (buf);
        }
    }

    // Bus compressor - wrap L/R as a 2-channel AudioBuffer view and process
    // in place. UniversalCompressor::processBlock only resizes internal
    // sidechain/lookahead scratch; it never replaces the input pointers, so
    // the prior copy-in/copy-out through compStereoBuffer was wasted cache
    // pressure. We still chunk by preparedBlockSize so an oversized
    // host-driven numSamples can't overflow the comp's internals.
    {
        const int bufSize = compStereoBuffer.getNumSamples();
        updateCompParameters();
        for (int offset = 0; offset < numSamples; offset += bufSize)
        {
            const int n = juce::jmin (bufSize, numSamples - offset);
            float* lrView[2] = { L + offset, R + offset };
            juce::AudioBuffer<float> compBuf (lrView, 2, n);
            compMidi.clear();
            busComp.processBlock (compBuf, compMidi);
        }

        if (paramsRef != nullptr)
            paramsRef->meterGrDb.store (-busComp.getGainReduction(),
                                         std::memory_order_relaxed);
    }
#endif

#if ADHDAW_HAS_DUSK_DSP
    if (tapeOn && useOversampler)
    {
        float* channels[2] = { L, R };
        juce::dsp::AudioBlock<float> block (channels, 2, (size_t) numSamples);
        auto upBlock = oversampler->processSamplesUp (block);

        const auto upSamples = (int) upBlock.getNumSamples();
        float* upChannels[2] = { upBlock.getChannelPointer (0),
                                  upBlock.getChannelPointer (1) };
        juce::AudioBuffer<float> upBuf (upChannels, 2, upSamples);
        tape.process (upBuf);
        oversampler->processSamplesDown (block);
    }
    else if (tapeOn)
    {
        float* channels[2] = { L, R };
        juce::AudioBuffer<float> tapeBuf (channels, 2, numSamples);
        tape.process (tapeBuf);
    }
#endif

    float postPeakL = 0.0f, postPeakR = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float g = faderGain.getNextValue();
        L[i] *= g;
        R[i] *= g;
        const float aL = std::fabs (L[i]);
        const float aR = std::fabs (R[i]);
        if (aL > postPeakL) postPeakL = aL;
        if (aR > postPeakR) postPeakR = aR;
    }

    if (paramsRef != nullptr)
    {
        const auto toDb = [] (float a) { return a > 1.0e-5f
            ? juce::Decibels::gainToDecibels (a, -100.0f) : -100.0f; };
        paramsRef->meterPostMasterLDb.store (toDb (postPeakL), std::memory_order_relaxed);
        paramsRef->meterPostMasterRDb.store (toDb (postPeakR), std::memory_order_relaxed);
    }
}
} // namespace adhdaw
