#include "BusStrip.h"
#include <cmath>
#include <cstring>

namespace focal
{
void BusStrip::bind (const BusParams& params) noexcept
{
    paramsRef = &params;
}

void BusStrip::prepare (double sampleRate, int blockSize, int oversamplingFactor)
{
    faderGain.reset (sampleRate, 0.020);
    faderGain.setCurrentAndTargetValue (1.0f);
    panGainL .reset (sampleRate, 0.020);
    panGainR .reset (sampleRate, 0.020);
    panGainL .setCurrentAndTargetValue (1.0f);
    panGainR .setCurrentAndTargetValue (1.0f);

#if FOCAL_HAS_DUSK_DSP
    eq.prepare (sampleRate, juce::jmax (1, blockSize), 2);
    eq.reset();

    busComp.setPlayConfigDetails (2, 2, sampleRate, juce::jmax (1, blockSize));
    busComp.prepareToPlay (sampleRate, juce::jmax (1, blockSize));
    // Internal oversampling tracks the global factor - same approach as
    // MasterBus. 1× → off (default), 2× / 4× → on (donor's internal 2×).
    busComp.setInternalOversamplingEnabled (oversamplingFactor > 1);
    compStereoBuffer.setSize (2, juce::jmax (1, blockSize), false, false, true);
    compMidi.clear();
    bindCompParams();
#endif
}

#if FOCAL_HAS_DUSK_DSP
void BusStrip::bindCompParams() noexcept
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

    storeAtom (compModeAtom, 3.0f);            // Bus mode
    storeAtom (compMixAtom,         100.0f);
    storeAtom (compBusMixAtom,      100.0f);
    storeAtom (compAutoMakeupAtom,    0.0f);
}

void BusStrip::updateEqParameters() noexcept
{
    if (paramsRef == nullptr) return;
    // Value-init padding to zero so the memcmp cache against lastEqParams
    // is reliable - see ChannelStrip equivalent for full reasoning.
    BritishEQProcessor::Parameters p {};
    p.hpfEnabled = false; p.hpfFreq = 80.0f;
    p.lpfEnabled = false; p.lpfFreq = 20000.0f;

    p.lfGain  = paramsRef->eqLfGainDb.load  (std::memory_order_relaxed);
    p.lfFreq  = 100.0f;
    p.lfBell  = false;  // shelf

    p.lmGain  = paramsRef->eqMidGainDb.load (std::memory_order_relaxed);
    p.lmFreq  = 1000.0f;
    p.lmQ     = 0.7f;

    p.hmGain  = 0.0f;  // HM unused; Bus EQ exposes only LF / MID / HF.
    p.hmFreq  = 4000.0f;
    p.hmQ     = 0.7f;

    p.hfGain  = paramsRef->eqHfGainDb.load  (std::memory_order_relaxed);
    p.hfFreq  = 8000.0f;
    p.hfBell  = false;

    p.isBlackMode = false;
    p.saturation  = 0.0f;
    p.inputGain   = 0.0f;
    p.outputGain  = 0.0f;
    if (std::memcmp (&p, &lastEqParams, sizeof (p)) != 0)
    {
        eq.setParameters (p);
        lastEqParams = p;
    }
}

void BusStrip::updateCompParameters() noexcept
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

void BusStrip::updateGainTargets() noexcept
{
    if (paramsRef == nullptr) return;

    const float faderDb = paramsRef->faderDb.load (std::memory_order_relaxed);
    const float gain = (faderDb <= ChannelStripParams::kFaderInfThreshDb)
                       ? 0.0f
                       : juce::Decibels::decibelsToGain (faderDb);
    faderGain.setTargetValue (gain);

    // Equal-power L/R balance - pan -1..1 → angle 0..pi/2.
    const float p     = juce::jlimit (-1.0f, 1.0f, paramsRef->pan.load (std::memory_order_relaxed));
    const float angle = (p + 1.0f) * (juce::MathConstants<float>::halfPi * 0.5f);
    panGainL.setTargetValue (std::cos (angle) * juce::MathConstants<float>::sqrt2);
    panGainR.setTargetValue (std::sin (angle) * juce::MathConstants<float>::sqrt2);
}

void BusStrip::processInPlace (float* L, float* R, int numSamples) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    if (numSamples == 0) return;

   #if FOCAL_HAS_DUSK_DSP
    // Contract: numSamples must fit the buffer prepare() sized for the comp.
    // The chunk loop below is the production safety net.
    jassert (numSamples <= compStereoBuffer.getNumSamples());
   #endif

    updateGainTargets();

#if FOCAL_HAS_DUSK_DSP
    {
        float* channels[2] = { L, R };
        juce::AudioBuffer<float> buf (channels, 2, numSamples);
        updateEqParameters();
        eq.process (buf);
    }

    // Wrap L/R directly as a 2-channel AudioBuffer view and let the comp
    // process in place - UniversalCompressor::processBlock only mutates
    // internal scratch, not the input buffer's structure. Chunk by prepared
    // block size to keep the comp's internal buffers from overflowing on
    // host-driven oversized blocks.
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

    float postPeakL = 0.0f, postPeakR = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float fg = faderGain.getNextValue();
        const float gL = panGainL.getNextValue() * fg;
        const float gR = panGainR.getNextValue() * fg;
        L[i] *= gL;
        R[i] *= gR;
        const float aL = std::fabs (L[i]);
        const float aR = std::fabs (R[i]);
        if (aL > postPeakL) postPeakL = aL;
        if (aR > postPeakR) postPeakR = aR;
    }

    if (paramsRef != nullptr)
    {
        const auto toDb = [] (float a) { return a > 1.0e-5f
            ? juce::Decibels::gainToDecibels (a, -100.0f) : -100.0f; };
        paramsRef->meterPostBusLDb.store (toDb (postPeakL), std::memory_order_relaxed);
        paramsRef->meterPostBusRDb.store (toDb (postPeakR), std::memory_order_relaxed);
    }
}
} // namespace focal
