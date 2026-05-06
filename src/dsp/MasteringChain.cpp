#include "MasteringChain.h"
#include <cstring>

namespace adhdaw
{
void MasteringChain::bind (const MasteringParams& params) noexcept
{
    paramsRef = &params;
}

void MasteringChain::prepare (double sampleRate, int blockSize)
{
    const int bs = juce::jmax (1, blockSize);

    digitalEq.prepare (sampleRate, bs);
    digitalEq.reset();

#if ADHDAW_HAS_DUSK_DSP
    busComp.setPlayConfigDetails (2, 2, sampleRate, bs);
    busComp.prepareToPlay (sampleRate, bs);
    compStereoBuffer.setSize (2, bs, false, false, true);
    compMidi.clear();
    bindCompParams();
#endif

    limiter.prepare (sampleRate, bs);
    limiter.reset();

    loudnessMeter.prepare (sampleRate, bs);

    preparedBlockSize = bs;
}

void MasteringChain::resetLoudness()
{
    loudnessMeter.reset();
}

#if ADHDAW_HAS_DUSK_DSP
void MasteringChain::bindCompParams()
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

    // Multiband mode (CompressorMode::Multiband = 7) - 4 bands with
    // Linkwitz-Riley LR4 crossovers at default 200 / 2000 / 8000 Hz.
    // The bus_* params no longer drive anything in this mode; per-band
    // controls live under the mb_<bandname>_* APVTS keys (built out by
    // the mastering UI in a follow-up iteration).
    storeAtom (compModeAtom, 7.0f);
    storeAtom (compMixAtom,         100.0f);
    storeAtom (compBusMixAtom,      100.0f);
    storeAtom (compAutoMakeupAtom,    0.0f);
}

void MasteringChain::updateCompParameters() noexcept
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

// MasteringDigitalEq is built unconditionally - its updater must be too,
// otherwise the link fails when ADHDAW_HAS_DUSK_DSP is off (the call site
// in processInPlace is unconditional and the prototype lives outside the
// macro guard in the header).
void MasteringChain::updateEqParameters() noexcept
{
    if (paramsRef == nullptr) return;

    digitalEq.setEnabled (paramsRef->eqEnabled.load (std::memory_order_relaxed));
    for (int b = 0; b < MasteringParams::kNumEqBands; ++b)
    {
        digitalEq.setBandFreq   (b, paramsRef->eqBandFreq  [b].load (std::memory_order_relaxed));
        digitalEq.setBandGainDb (b, paramsRef->eqBandGainDb[b].load (std::memory_order_relaxed));
        digitalEq.setBandQ      (b, paramsRef->eqBandQ     [b].load (std::memory_order_relaxed));
    }
}

void MasteringChain::updateLimiterParameters() noexcept
{
    if (paramsRef == nullptr) return;
    limiter.setEnabled    (paramsRef->limiterEnabled.load (std::memory_order_relaxed));
    limiter.setInputDriveDb (paramsRef->limiterDriveDb.load (std::memory_order_relaxed));
    limiter.setCeilingDb  (paramsRef->limiterCeilingDb.load (std::memory_order_relaxed));
    limiter.setReleaseMs  (paramsRef->limiterReleaseMs.load (std::memory_order_relaxed));
}

void MasteringChain::processInPlace (float* L, float* R, int numSamples) noexcept
{
    jassert (numSamples <= preparedBlockSize);

    // 5-band digital EQ - replaces the Tube EQ that the master strip
    // uses in Mixing. Mastering wants a clean parametric EQ.
    updateEqParameters();
    digitalEq.processInPlace (L, R, numSamples);

#if ADHDAW_HAS_DUSK_DSP
    {
        const int bufSize = compStereoBuffer.getNumSamples();
        updateCompParameters();
        for (int offset = 0; offset < numSamples; offset += bufSize)
        {
            const int n = juce::jmin (bufSize, numSamples - offset);
            compStereoBuffer.copyFrom (0, 0, L + offset, n);
            compStereoBuffer.copyFrom (1, 0, R + offset, n);
            compMidi.clear();
            busComp.processBlock (compStereoBuffer, compMidi);
            std::memcpy (L + offset, compStereoBuffer.getReadPointer (0),
                         sizeof (float) * (size_t) n);
            std::memcpy (R + offset, compStereoBuffer.getReadPointer (1),
                         sizeof (float) * (size_t) n);
        }
        if (paramsRef != nullptr)
            paramsRef->meterCompGrDb.store (-busComp.getGainReduction(),
                                             std::memory_order_relaxed);
    }
#endif

    updateLimiterParameters();
    limiter.processInPlace (L, R, numSamples);

    if (paramsRef != nullptr)
        paramsRef->meterLimiterGrDb.store (limiter.getCurrentGrDb(),
                                            std::memory_order_relaxed);

    // Output meters - peak per channel post-limiter (equivalent to "true
    // peak" at sample resolution; ISP is a follow-up).
    float peakL = 0.0f, peakR = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float aL = std::fabs (L[i]);
        const float aR = std::fabs (R[i]);
        if (aL > peakL) peakL = aL;
        if (aR > peakR) peakR = aR;
    }
    if (paramsRef != nullptr)
    {
        const auto toDb = [] (float a) { return a > 1.0e-5f
            ? juce::Decibels::gainToDecibels (a, -100.0f) : -100.0f; };
        paramsRef->meterPostMasterLDb.store (toDb (peakL), std::memory_order_relaxed);
        paramsRef->meterPostMasterRDb.store (toDb (peakR), std::memory_order_relaxed);
    }

    // Loudness - measured on the post-limiter, post-output signal so the
    // user sees what's actually committed when they Export.
    loudnessMeter.process (L, R, numSamples);
    if (paramsRef != nullptr)
    {
        paramsRef->meterMomentaryLufs.store  (loudnessMeter.getMomentaryLufs(),  std::memory_order_relaxed);
        paramsRef->meterShortTermLufs.store  (loudnessMeter.getShortTermLufs(),  std::memory_order_relaxed);
        paramsRef->meterIntegratedLufs.store (loudnessMeter.getIntegratedLufs(), std::memory_order_relaxed);
        paramsRef->meterTruePeakDb.store     (loudnessMeter.getTruePeakDb(),    std::memory_order_relaxed);
    }
}
} // namespace adhdaw
