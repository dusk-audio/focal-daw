#include "ChannelStrip.h"
#include <cmath>
#include <cstring>

namespace focal
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
    for (auto& s : auxSendGain)
    {
        s.reset (sampleRate, rampSeconds);
        s.setCurrentAndTargetValue (0.0f);
    }
    for (auto& b : auxSendPre) b = false;

    tempMono.assign ((size_t) juce::jmax (1, blockSize), 0.0f);

    // Plugin slot - prepared at the same SR/BS so the audio thread never
    // sees an unprepared instance. If the slot has no plugin loaded, this
    // is essentially a no-op; if a plugin is loaded across a device-rate
    // change, the slot re-preps it for the new config.
    pluginSlot.prepareToPlay (sampleRate, juce::jmax (1, blockSize));

#if FOCAL_HAS_DUSK_DSP
    // BritishEQProcessor.prepare expects (sampleRate, blockSize, numChannels).
    eq.prepare (sampleRate, juce::jmax (1, blockSize), 1);
    eq.reset();

    // ChannelComp: thin facade over UniversalCompressor with minimal-processing
    // fast path enabled at construction. One instance per channel.
    compressor.prepare (sampleRate, juce::jmax (1, blockSize), 1);
    compMonoBuffer.setSize (1, juce::jmax (1, blockSize), false, false, true);
    bindCompParams();
#endif
}

#if FOCAL_HAS_DUSK_DSP
void ChannelStrip::bindCompParams()
{
    auto& apvts = compressor.getParameters();
    // getRawParameterValue() returns a pointer to the parameter's denormalised
    // value atomic - the same atomic that UniversalCompressor's processBlock()
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

    // Read liveFaderDb, not faderDb: AudioEngine routes the effective dB
    // (manual setpoint OR Read-mode automation) through this atom each
    // block. faderDb stays the persisted user setpoint.
    const float faderDb = paramsRef->liveFaderDb.load (std::memory_order_relaxed);
    const bool  muted   = paramsRef->mute.load        (std::memory_order_relaxed);

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

    // Aux sends - per-knob linear gain. -100 dB sentinel (knob fully CCW)
    // hard-mutes; the inner loop short-circuits zero-gain sends.
    for (int i = 0; i < kNumAuxSends; ++i)
    {
        const float db = paramsRef->auxSendDb[(size_t) i].load (std::memory_order_relaxed);
        const float g  = (db <= ChannelStripParams::kAuxSendOffDb)
                            ? 0.0f
                            : juce::Decibels::decibelsToGain (db);
        auxSendGain[(size_t) i].setTargetValue (g);
        auxSendPre[(size_t) i] =
            paramsRef->auxSendPreFader[(size_t) i].load (std::memory_order_relaxed);
    }
}

void ChannelStrip::updateEqParameters() noexcept
{
#if FOCAL_HAS_DUSK_DSP
    if (paramsRef == nullptr) return;
    // Value-init so padding bytes are zero - paired with lastEqParams's {}
    // initializer, this lets memcmp tell us reliably whether the params
    // actually changed since last block. Skipping setParameters() when they
    // haven't avoids a full BritishEQProcessor coefficient recompute (8-14
    // biquads) on every silent block on every channel.
    BritishEQProcessor::Parameters p {};
    p.hpfEnabled = paramsRef->hpfEnabled.load (std::memory_order_relaxed);
    p.hpfFreq    = paramsRef->hpfFreq.load    (std::memory_order_relaxed);
    // LPF stays disabled at the per-channel level - the strip doesn't expose one.
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
    if (std::memcmp (&p, &lastEqParams, sizeof (p)) != 0)
    {
        eq.setParameters (p);
        lastEqParams = p;
    }
#endif
}

void ChannelStrip::updateCompParameters() noexcept
{
#if FOCAL_HAS_DUSK_DSP
    if (paramsRef == nullptr) return;

    // Direct atomic stores - no lock, no message-thread notification, no
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
                                         const std::array<float*, kNumAuxSends>& auxLaneL,
                                         const std::array<float*, kNumAuxSends>& auxLaneR,
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
        for (auto& s : busGain)     s.setTargetValue (0.0f);
        for (auto& s : auxSendGain) s.setTargetValue (0.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            faderGain.getNextValue();
            for (auto& s : busGain)     s.getNextValue();
            for (auto& s : auxSendGain) s.getNextValue();
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
        for (auto& s : busGain)     s.setTargetValue (0.0f);
        for (auto& s : auxSendGain) s.setTargetValue (0.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            faderGain.getNextValue();
            for (auto& s : busGain)     s.getNextValue();
            for (auto& s : auxSendGain) s.getNextValue();
        }
        return;
    }

    updateGainTargets();
    updateEqParameters();
    updateCompParameters();

    std::memcpy (tempMono.data(), monoIn, sizeof (float) * (size_t) numSamples);

    // Phase invert (Ø) - flip polarity before EQ/comp/fader so the rest of
    // the chain sees the corrected-polarity signal. SIMD'd negate via JUCE
    // saves a per-sample scalar multiply when phase invert is engaged.
    if (paramsRef->phaseInvert.load (std::memory_order_relaxed))
        juce::FloatVectorOperations::negate (tempMono.data(), tempMono.data(), numSamples);

    // Per-channel insert plugin (post-phase-invert, pre-EQ). No-op when no
    // plugin loaded or slot bypassed; lock-free atomic read of the instance
    // pointer otherwise.
    pluginSlot.processMonoBlock (tempMono.data(), numSamples);

#if FOCAL_HAS_DUSK_DSP
    // Wrap our mono scratch buffer as a 1-channel juce::AudioBuffer<float>
    // (no allocation - referenceTo points at the existing storage).
    float* monoChannel[1] = { tempMono.data() };
    juce::AudioBuffer<float> monoBuf (monoChannel, 1, numSamples);
    eq.process (monoBuf);

    // Compressor: UniversalCompressor::processBlock only mutates internal
    // sidechain/lookahead scratch - never the input buffer's size or channel
    // pointers. So we wrap tempMono directly as a 1-channel AudioBuffer view
    // and let the comp process in place, skipping the per-channel copy-in /
    // copy-out that used to push 2 × numSamples × 4 B through cache for
    // every active strip every callback.
    //
    // We still chunk by the prepared block size so a host-driven
    // numSamples > preparedBlockSize can't overflow the comp's internally
    // sized scratch buffers - the chunking loop below covers the oversized
    // case correctly, so no jassert here.
    {
        const int bufSize = compMonoBuffer.getNumSamples();
        for (int offset = 0; offset < numSamples; offset += bufSize)
        {
            const int n = juce::jmin (bufSize, numSamples - offset);
            float* monoView[1] = { tempMono.data() + offset };
            juce::AudioBuffer<float> compBuf (monoView, 1, n);
            compressor.processBlock (compBuf);
        }
    }
    currentGrDb.store (-compressor.getGainReductionDb(), std::memory_order_relaxed);
#endif

    // Expose the post-EQ/post-comp buffer for the recorder. Valid until the
    // next call to processAndAccumulate().
    lastProcessedPtr = tempMono.data();
    lastProcessedSamples = numSamples;

    if (! passByGate)
    {
        // Strip is muted/soloed-out/IN-off - keep the smoothers ticking but
        // don't accumulate to master/buses. The DSP STILL ran above, so a
        // recording-armed track with `printEffects` can still capture the
        // post-effects signal even when the engineer has IN off (direct
        // hardware monitoring scenario).
        faderGain.setTargetValue (0.0f);
        for (auto& s : busGain)     s.setTargetValue (0.0f);
        for (auto& s : auxSendGain) s.setTargetValue (0.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            faderGain.getNextValue();
            for (auto& s : busGain)     s.getNextValue();
            for (auto& s : auxSendGain) s.getNextValue();
        }
        return;
    }

    // Hoist the bus-active set out of the inner sample loop. The common
    // case (and the only case for a bare-bones session) is "no bus
    // assigned" - tracks pass straight to master. We branch once per
    // block on whether any bus is active OR smoothing; if not we run a
    // master-only fast path that skips the per-sample 4-bus scan and the
    // (1 - maxBusG) crossfade math entirely. Bus smoothers must still
    // tick to follow target so re-engaging a bus is click-free.
    bool anyBusActive = false;
    bool anyBusSmoothing = false;
    for (int a = 0; a < kNumBuses; ++a)
    {
        if (busGain[(size_t) a].getCurrentValue() > 0.0f) anyBusActive = true;
        if (busGain[(size_t) a].isSmoothing())            anyBusSmoothing = true;
    }
    bool anyAuxActive = false;
    bool anyAuxSmoothing = false;
    for (int a = 0; a < kNumAuxSends; ++a)
    {
        if (auxSendGain[(size_t) a].getCurrentValue() > 0.0f) anyAuxActive = true;
        if (auxSendGain[(size_t) a].isSmoothing())            anyAuxSmoothing = true;
    }

    if (! anyBusActive && ! anyBusSmoothing && ! anyAuxActive && ! anyAuxSmoothing)
    {
        // Master-only fast path - no bus, no aux. The common steady state
        // for a track that's just summing direct to master.
        for (int i = 0; i < numSamples; ++i)
        {
            const float fg = faderGain.getNextValue();
            const float gL = panGainL.getNextValue() * fg;
            const float gR = panGainR.getNextValue() * fg;
            const float sIn = tempMono[(size_t) i];
            masterL[i] += sIn * gL;
            masterR[i] += sIn * gR;
        }
        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const float fg = faderGain.getNextValue();
        // Capture pan-only gains so we can build pre-fader stereo for the
        // aux sends without ticking the smoother twice. wetL/wetR include
        // the fader; wetLPre/wetRPre are post-pan, pre-fader (used by aux
        // sends with auxSendPre[i]==true).
        const float pL = panGainL.getNextValue();
        const float pR = panGainR.getNextValue();
        const float gL = pL * fg;
        const float gR = pR * fg;
        const float sIn = tempMono[(size_t) i];
        const float wetLPre = sIn * pL;
        const float wetRPre = sIn * pR;
        const float wetL = sIn * gL;
        const float wetR = sIn * gR;

        // Bus routing is EXCLUSIVE with master routing: a track assigned to
        // any bus must not also hit the master direct, otherwise the signal
        // would arrive at master twice (once direct, once via the bus's own
        // sum-into-master in AudioEngine), producing a +3 dB doubling. We
        // gate the master accumulation by (1 - maxBusGain) so a fully-on
        // bus assignment fully removes the direct send, and a mid-ramp
        // toggle produces a smooth crossfade between routes.
        float perBusG[kNumBuses];
        float maxBusG = 0.0f;
        for (int a = 0; a < kNumBuses; ++a)
        {
            perBusG[a] = busGain[(size_t) a].getNextValue();
            if (perBusG[a] > maxBusG) maxBusG = perBusG[a];
        }
        const float toMaster = juce::jmax (0.0f, 1.0f - maxBusG);

        masterL[i] += wetL * toMaster;
        masterR[i] += wetR * toMaster;

        for (int a = 0; a < kNumBuses; ++a)
        {
            if (perBusG[a] > 0.0f)
            {
                busL[(size_t) a][i] += wetL * perBusG[a];
                busR[(size_t) a][i] += wetR * perBusG[a];
            }
        }

        // Aux sends. Each send's gain ticks every sample so a knob ramp is
        // smooth; pre/post-fader is captured per-block (auxSendPre[]). A
        // send is independent of bus-vs-master routing - turning up an aux
        // send doesn't reduce the master / bus signal, since auxes are FX
        // returns that mix back into master via their own AuxLaneStrip pass.
        for (int a = 0; a < kNumAuxSends; ++a)
        {
            const float sg = auxSendGain[(size_t) a].getNextValue();
            if (sg <= 0.0f) continue;
            if (auxSendPre[(size_t) a])
            {
                auxLaneL[(size_t) a][i] += wetLPre * sg;
                auxLaneR[(size_t) a][i] += wetRPre * sg;
            }
            else
            {
                auxLaneL[(size_t) a][i] += wetL * sg;
                auxLaneR[(size_t) a][i] += wetR * sg;
            }
        }
    }
}
} // namespace focal
