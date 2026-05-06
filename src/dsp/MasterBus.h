#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include "../session/Session.h"

#if FOCAL_HAS_DUSK_DSP
  #include "TapeSaturation.h"     // tape-echo/Source/DSP - TapeEchoDSP::TapeSaturation
  #include "TubeEQProcessor.h"    // multi-q - Pultec-style EQ
  #include "UniversalCompressor.h"// multi-comp - Bus mode for the master comp
#endif

namespace focal
{
// Phase 1a master bus: Pultec-style Tube EQ → bus compressor → tape
// saturation → master fader. Parameters come from session.master() via
// MasterBusParams; UI mutates the atomics, the audio thread reads them.
class MasterBus
{
public:
    MasterBus();

    // oversamplingFactor: 1 = native (default), 2 = 2× ox, 4 = 4× ox. Affects
    // the bus compressor's internal oversampling toggle and the tape sat
    // oversampler's stage count. Other values are clamped to 1.
    void prepare (double sampleRate, int blockSize, int oversamplingFactor = 1);
    void bind (const MasterBusParams& params) noexcept;

    void processInPlace (float* L, float* R, int numSamples) noexcept;

private:
    const MasterBusParams* paramsRef = nullptr;
    juce::SmoothedValue<float> faderGain { 1.0f };

    static constexpr float kTapeDrive = 0.5f;

#if FOCAL_HAS_DUSK_DSP
    TapeEchoDSP::TapeSaturation tape;
    TubeEQProcessor             tubeEQ;
    TubeEQProcessor::Parameters lastTubeEqParams {};   // see ChannelStrip equivalent
    UniversalCompressor         busComp;
    juce::MidiBuffer            compMidi;            // unused but required by processBlock
    juce::AudioBuffer<float>    compStereoBuffer;    // pre-allocated; comp processBlock target

    // Cached APVTS atoms for the bus compressor - written every block from
    // session params via direct atomic store (no setValueNotifyingHost), same
    // lock-free pattern as ChannelStrip.
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

    void bindCompParams();
    void updateEqParameters() noexcept;
    void updateCompParameters() noexcept;
    static inline void storeAtom (std::atomic<float>* a, float v) noexcept
    {
        if (a != nullptr) a->store (v, std::memory_order_relaxed);
    }
#endif

    // Tape-saturation oversampler. Rebuilt at prepare() time based on the
    // global oversampling factor: nullptr = 1× (no oversampling), 1 stage =
    // 2×, 2 stages = 4×. unique_ptr because juce::dsp::Oversampling has no
    // default constructor and the stage count must change between prepares.
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    int oversamplerStages    = 0;     // mirrors what oversampler was built with; 0 = 1×, 1 = 2×, 2 = 4×
    int preparedBlockSize    = 0;
    int currentOxFactor      = 1;     // 1, 2 or 4 - set in prepare()
};
} // namespace focal
