#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "../session/Session.h"
#include <atomic>

#if FOCAL_HAS_DUSK_DSP
  #include "BritishEQProcessor.h"
  #include "UniversalCompressor.h"
#endif

namespace focal
{
// Phase 1a aux bus: 3-band EQ → bus compressor → pan → fader → meter.
// EQ uses BritishEQProcessor's LF / LM / HF bands (with the LM band exposed
// as MID and the HM band fixed-zero). Comp uses UniversalCompressor's Bus
// mode. Both DSP instances are owned by the strip; APVTS atoms for the comp
// are cached in bind so updateCompParameters can write lock-free.
//
// Buses are subgroups (16 channels → 4 buses → master). They do NOT host
// plugins - that responsibility lives on the AUX return lanes accessed via
// the AUX stage UI.
class BusStrip
{
public:
    BusStrip() = default;

    // oversamplingFactor: 1 (native, default), 2 or 4. Toggles internal
    // oversampling on the bus comp.
    void prepare (double sampleRate, int blockSize, int oversamplingFactor = 1);
    void bind (const BusParams& params) noexcept;

    // Applies all bus DSP to L/R in place. Caller has already applied the
    // SIP gate (mute/solo) before invoking.
    void processInPlace (float* L, float* R, int numSamples) noexcept;

private:
    const BusParams* paramsRef = nullptr;
    juce::SmoothedValue<float> faderGain { 1.0f };
    juce::SmoothedValue<float> panGainL  { 1.0f };
    juce::SmoothedValue<float> panGainR  { 1.0f };

#if FOCAL_HAS_DUSK_DSP
    BritishEQProcessor       eq;
    BritishEQProcessor::Parameters lastEqParams {};   // see ChannelStrip equivalent
    UniversalCompressor      busComp;
    juce::MidiBuffer         compMidi;
    juce::AudioBuffer<float> compStereoBuffer;

    std::atomic<float>* compModeAtom        = nullptr;
    std::atomic<float>* compBypassAtom      = nullptr;
    std::atomic<float>* compMixAtom         = nullptr;
    std::atomic<float>* compAutoMakeupAtom  = nullptr;
    std::atomic<float>* compBusThreshAtom   = nullptr;
    std::atomic<float>* compBusRatioAtom    = nullptr;
    std::atomic<float>* compBusAttackAtom   = nullptr;
    std::atomic<float>* compBusReleaseAtom  = nullptr;
    std::atomic<float>* compBusMakeupAtom   = nullptr;
    std::atomic<float>* compBusMixAtom      = nullptr;

    void bindCompParams() noexcept;
    void updateEqParameters() noexcept;
    void updateCompParameters() noexcept;
    static inline void storeAtom (std::atomic<float>* a, float v) noexcept
    {
        if (a != nullptr) a->store (v, std::memory_order_relaxed);
    }
#endif

    void updateGainTargets() noexcept;
};
} // namespace focal
