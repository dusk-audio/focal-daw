#include "ChannelStrip.h"
#include <cmath>
#include <cstring>

namespace adhdaw
{
void ChannelStrip::prepare (double sampleRate, int blockSize)
{
    constexpr double rampSeconds = 0.020;
    faderGain.reset (sampleRate, rampSeconds);
    panGainL.reset  (sampleRate, rampSeconds);
    panGainR.reset  (sampleRate, rampSeconds);
    faderGain.setCurrentAndTargetValue (0.0f);
    panGainL.setCurrentAndTargetValue (0.7071f);
    panGainR.setCurrentAndTargetValue (0.7071f);
    for (auto& s : busGain)
    {
        s.reset (sampleRate, rampSeconds);
        s.setCurrentAndTargetValue (0.0f);
    }

    tempMono.assign ((size_t) juce::jmax (1, blockSize), 0.0f);

#if ADHDAW_HAS_DUSK_DSP
    // BritishEQProcessor.prepare expects (sampleRate, blockSize, numChannels).
    eq.prepare (sampleRate, juce::jmax (1, blockSize), 1);
    eq.reset();

    // UniversalCompressor: configure as 1-channel mono processor.
    compressor.setPlayConfigDetails (1, 1, sampleRate, juce::jmax (1, blockSize));
    compressor.prepareToPlay (sampleRate, juce::jmax (1, blockSize));
    compMonoBuffer.setSize (1, juce::jmax (1, blockSize), false, false, true);
    compMidi.clear();
    bindCompParams();
#endif
}

#if ADHDAW_HAS_DUSK_DSP
void ChannelStrip::bindCompParams()
{
    auto& apvts = compressor.getParameters();
    // getRawParameterValue() returns a pointer to the parameter's denormalised
    // value atomic — the same atomic that UniversalCompressor's processBlock()
    // reads. Writing here is lock-free and notification-free, suitable for the
    // audio thread. Stores hold SI-unit / index / 0-or-1 values.
    compModeAtom        = apvts.getRawParameterValue ("mode");
    compBypassAtom      = apvts.getRawParameterValue ("bypass");
    compMixAtom         = apvts.getRawParameterValue ("mix");
    compAutoMakeupAtom  = apvts.getRawParameterValue ("auto_makeup");
    compOptoPeakRedAtom = apvts.getRawParameterValue ("opto_peak_reduction");
    compOptoGainAtom    = apvts.getRawParameterValue ("opto_gain");
    compOptoLimitAtom   = apvts.getRawParameterValue ("opto_limit");
    compFetInputAtom    = apvts.getRawParameterValue ("fet_input");
    compFetOutputAtom   = apvts.getRawParameterValue ("fet_output");
    compFetAttackAtom   = apvts.getRawParameterValue ("fet_attack");
    compFetReleaseAtom  = apvts.getRawParameterValue ("fet_release");
    compFetRatioAtom    = apvts.getRawParameterValue ("fet_ratio");
    compVcaThreshAtom   = apvts.getRawParameterValue ("vca_threshold");
    compVcaRatioAtom    = apvts.getRawParameterValue ("vca_ratio");
    compVcaAttackAtom   = apvts.getRawParameterValue ("vca_attack");
    compVcaReleaseAtom  = apvts.getRawParameterValue ("vca_release");
    compVcaOutputAtom   = apvts.getRawParameterValue ("vca_output");

    // Mix=100% wet, auto-makeup off (we control makeup via per-mode output param).
    storeAtom (compMixAtom,        100.0f);
    storeAtom (compAutoMakeupAtom,   0.0f);  // Choice index 0 = "Off"
}
#endif

void ChannelStrip::updateGainTargets() noexcept
{
    if (paramsRef == nullptr) return;

    const float faderDb = paramsRef->faderDb.load (std::memory_order_relaxed);
    const bool  muted   = paramsRef->mute.load   (std::memory_order_relaxed);

    const float gain = (muted || faderDb <= ChannelStripParams::kFaderInfThreshDb)
                       ? 0.0f
                       : juce::Decibels::decibelsToGain (faderDb);
    faderGain.setTargetValue (gain);

    const float p     = juce::jlimit (-1.0f, 1.0f, paramsRef->pan.load (std::memory_order_relaxed));
    const float angle = (p + 1.0f) * (juce::MathConstants<float>::halfPi * 0.5f);
    panGainL.setTargetValue (std::cos (angle));
    panGainR.setTargetValue (std::sin (angle));

    for (int i = 0; i < kNumBuses; ++i)
        busGain[(size_t) i].setTargetValue (
            paramsRef->busAssign[(size_t) i].load (std::memory_order_relaxed) ? 1.0f : 0.0f);
}

void ChannelStrip::updateEqParameters() noexcept
{
#if ADHDAW_HAS_DUSK_DSP
    if (paramsRef == nullptr) return;
    BritishEQProcessor::Parameters p;
    p.hpfEnabled = paramsRef->hpfEnabled.load (std::memory_order_relaxed);
    p.hpfFreq    = paramsRef->hpfFreq.load    (std::memory_order_relaxed);
    // LPF stays disabled at the per-channel level — the strip doesn't expose one.
    p.lpfEnabled = false;
    p.lpfFreq    = 20000.0f;
    p.lfGain     = paramsRef->lfGainDb.load (std::memory_order_relaxed);
    p.lfFreq     = paramsRef->lfFreq.load   (std::memory_order_relaxed);
    p.lfBell     = false;
    p.lmGain     = paramsRef->lmGainDb.load (std::memory_order_relaxed);
    p.lmFreq     = paramsRef->lmFreq.load   (std::memory_order_relaxed);
    p.lmQ        = paramsRef->lmQ.load      (std::memory_order_relaxed);
    p.hmGain     = paramsRef->hmGainDb.load (std::memory_order_relaxed);
    p.hmFreq     = paramsRef->hmFreq.load   (std::memory_order_relaxed);
    p.hmQ        = paramsRef->hmQ.load      (std::memory_order_relaxed);
    p.hfGain     = paramsRef->hfGainDb.load (std::memory_order_relaxed);
    p.hfFreq     = paramsRef->hfFreq.load   (std::memory_order_relaxed);
    p.hfBell     = false;
    p.isBlackMode = paramsRef->eqBlackMode.load (std::memory_order_relaxed);
    p.saturation  = 0.0f;
    p.inputGain   = 0.0f;
    p.outputGain  = 0.0f;
    eq.setParameters (p);
#endif
}

void ChannelStrip::updateCompParameters() noexcept
{
#if ADHDAW_HAS_DUSK_DSP
    if (paramsRef == nullptr) return;

    // Direct atomic stores — no lock, no message-thread notification, no
    // queue traffic. UniversalCompressor's processBlock() reads from the same
    // atomics. ~17 stores per channel × 16 channels per block, each ~4 ns.
    storeAtom (compBypassAtom,
               paramsRef->compEnabled.load (std::memory_order_relaxed) ? 0.0f : 1.0f);

    const int modeIdx = juce::jlimit (0, 2, paramsRef->compMode.load (std::memory_order_relaxed));
    storeAtom (compModeAtom, (float) modeIdx);

    storeAtom (compOptoPeakRedAtom,
               paramsRef->compOptoPeakRed.load (std::memory_order_relaxed));
    storeAtom (compOptoGainAtom,
               paramsRef->compOptoGain.load    (std::memory_order_relaxed));
    storeAtom (compOptoLimitAtom,
               paramsRef->compOptoLimit.load   (std::memory_order_relaxed) ? 1.0f : 0.0f);

    storeAtom (compFetInputAtom,   paramsRef->compFetInput.load   (std::memory_order_relaxed));
    storeAtom (compFetOutputAtom,  paramsRef->compFetOutput.load  (std::memory_order_relaxed));
    storeAtom (compFetAttackAtom,  paramsRef->compFetAttack.load  (std::memory_order_relaxed));
    storeAtom (compFetReleaseAtom, paramsRef->compFetRelease.load (std::memory_order_relaxed));
    storeAtom (compFetRatioAtom,
               (float) paramsRef->compFetRatio.load (std::memory_order_relaxed));

    storeAtom (compVcaThreshAtom,  paramsRef->compVcaThreshDb.load (std::memory_order_relaxed));
    storeAtom (compVcaRatioAtom,   paramsRef->compVcaRatio.load    (std::memory_order_relaxed));
    storeAtom (compVcaAttackAtom,  paramsRef->compVcaAttack.load   (std::memory_order_relaxed));
    storeAtom (compVcaReleaseAtom, paramsRef->compVcaRelease.load  (std::memory_order_relaxed));
    storeAtom (compVcaOutputAtom,  paramsRef->compVcaOutput.load   (std::memory_order_relaxed));
#endif
}

void ChannelStrip::processAndAccumulate (const float* monoIn,
                                         float* masterL, float* masterR,
                                         const std::array<float*, kNumBuses>& busL,
                                         const std::array<float*, kNumBuses>& busR,
                                         int numSamples,
                                         bool passByGate) noexcept
{
    lastProcessedPtr = nullptr;
    lastProcessedSamples = 0;

    // No params or no input → tick the smoothers down (so M/S transitions
    // sound smooth) and bail. The strip produces silence and exposes no
    // processed buffer to the recorder.
    if (paramsRef == nullptr || monoIn == nullptr)
    {
        faderGain.setTargetValue (0.0f);
        for (auto& s : busGain) s.setTargetValue (0.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            faderGain.getNextValue();
            for (auto& s : busGain) s.getNextValue();
        }
        return;
    }

    if ((int) tempMono.size() < numSamples)
        return;  // can't allocate on the audio thread; bail safely (silence)

    // Skip the heavy DSP when the strip isn't passing to master and the
    // recorder doesn't need a processed buffer either. With 16 channels each
    // hosting a UniversalCompressor (a full juce::AudioProcessor), running
    // the chain on every silent track was an xrun-class CPU spike.
    if (! passByGate && ! needsProcessedMono)
    {
        faderGain.setTargetValue (0.0f);
        for (auto& s : busGain) s.setTargetValue (0.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            faderGain.getNextValue();
            for (auto& s : busGain) s.getNextValue();
        }
        return;
    }

    updateGainTargets();
    updateEqParameters();
    updateCompParameters();

    std::memcpy (tempMono.data(), monoIn, sizeof (float) * (size_t) numSamples);

    // Phase invert (Ø) — flip polarity before EQ/comp/fader so the rest of
    // the chain sees the corrected-polarity signal.
    if (paramsRef->phaseInvert.load (std::memory_order_relaxed))
        for (int i = 0; i < numSamples; ++i)
            tempMono[(size_t) i] = -tempMono[(size_t) i];

#if ADHDAW_HAS_DUSK_DSP
    // Wrap our mono scratch buffer as a 1-channel juce::AudioBuffer<float>
    // (no allocation — referenceTo points at the existing storage).
    float* monoChannel[1] = { tempMono.data() };
    juce::AudioBuffer<float> monoBuf (monoChannel, 1, numSamples);
    eq.process (monoBuf);

    // Compressor: copy into the comp's pre-allocated buffer (since
    // UniversalCompressor::processBlock may resize/replace channels), process,
    // then copy back into tempMono.
    {
        const int n = juce::jmin (numSamples, compMonoBuffer.getNumSamples());
        compMonoBuffer.copyFrom (0, 0, tempMono.data(), n);
        compMidi.clear();
        compressor.processBlock (compMonoBuffer, compMidi);
        std::memcpy (tempMono.data(), compMonoBuffer.getReadPointer (0),
                     sizeof (float) * (size_t) n);
    }
    currentGrDb.store (-compressor.getGainReduction(), std::memory_order_relaxed);
#endif

    // Expose the post-EQ/post-comp buffer for the recorder. Valid until the
    // next call to processAndAccumulate().
    lastProcessedPtr = tempMono.data();
    lastProcessedSamples = numSamples;

    if (! passByGate)
    {
        // Strip is muted/soloed-out/IN-off — keep the smoothers ticking but
        // don't accumulate to master/buses. The DSP STILL ran above, so a
        // recording-armed track with `printEffects` can still capture the
        // post-effects signal even when the engineer has IN off (direct
        // hardware monitoring scenario).
        faderGain.setTargetValue (0.0f);
        for (auto& s : busGain) s.setTargetValue (0.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            faderGain.getNextValue();
            for (auto& s : busGain) s.getNextValue();
        }
        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const float fg = faderGain.getNextValue();
        const float gL = panGainL.getNextValue() * fg;
        const float gR = panGainR.getNextValue() * fg;
        const float sIn = tempMono[(size_t) i];
        const float wetL = sIn * gL;
        const float wetR = sIn * gR;

        masterL[i] += wetL;
        masterR[i] += wetR;

        for (int a = 0; a < kNumBuses; ++a)
        {
            const float bg = busGain[(size_t) a].getNextValue();
            if (bg > 0.0f)
            {
                busL[(size_t) a][i] += wetL * bg;
                busR[(size_t) a][i] += wetR * bg;
            }
        }
    }
}
} // namespace adhdaw
