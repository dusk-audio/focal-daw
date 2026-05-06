#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
#include "../session/Session.h"
#include "../engine/PluginSlot.h"

#if FOCAL_HAS_DUSK_DSP
  #include "BritishEQProcessor.h"
  #include "UniversalCompressor.h"
#endif

namespace focal
{
// Phase 1a aux bus: 3-band EQ → bus compressor → send-FX plugin → pan →
// fader → meter. EQ uses BritishEQProcessor's LF / LM / HF bands (with the
// LM band exposed as MID and the HM band fixed-zero). Comp uses
// UniversalCompressor's Bus mode. The plugin slot (Phase 1b) hosts a single
// reverb / delay / etc. that processes the summed bus signal stereo-in,
// stereo-out before the pan/fader stage; same audio-thread-safe swap as the
// per-channel insert slots, so loading mid-playback is glitch-free.
class AuxBusStrip
{
public:
    AuxBusStrip() = default;

    // oversamplingFactor: 1 (native, default), 2 or 4. Toggles internal
    // oversampling on the bus comp.
    void prepare (double sampleRate, int blockSize, int oversamplingFactor = 1);
    void bind (const AuxBusParams& params) noexcept;

    // Bind the per-app PluginManager so this aux's PluginSlot can resolve
    // plugin files. Mirrors the channel-strip binding done at engine
    // construction. The slot stays dormant until a plugin is loaded.
    void bindPluginManager (PluginManager& mgr) noexcept { pluginSlot.setManager (mgr); }
    PluginSlot&       getPluginSlot()       noexcept { return pluginSlot; }
    const PluginSlot& getPluginSlot() const noexcept { return pluginSlot; }

    // Applies all bus DSP to L/R in place. Caller has already applied the
    // SIP gate (mute/solo) before invoking.
    void processInPlace (float* L, float* R, int numSamples) noexcept;

private:
    const AuxBusParams* paramsRef = nullptr;
    juce::SmoothedValue<float> faderGain { 1.0f };
    juce::SmoothedValue<float> panGainL  { 1.0f };
    juce::SmoothedValue<float> panGainR  { 1.0f };

    // Send-FX plugin slot. Sits between the bus comp and the pan/fader so the
    // reverb / delay sees the post-EQ/post-comp summed signal, but the user
    // can still trim its return level with the bus fader. Stereo in / stereo
    // out via PluginSlot::processStereoBlock.
    PluginSlot pluginSlot;

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

    void bindCompParams();
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
