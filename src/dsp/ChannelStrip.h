#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <vector>
#include "../session/Session.h"
#include "../engine/PluginSlot.h"

#if FOCAL_HAS_DUSK_DSP
  #include "BritishEQProcessor.h"   // multi-q
  // ChannelComp is a thin facade over UniversalCompressor with the donor's
  // minimal-processing fast path enabled. Same Opto/FET/VCA per-sample DSP,
  // but no internal oversampling, no sidechain HP/EQ, no true-peak detector,
  // no transient shaper, no global lookahead, no mix wet/dry, no auto-makeup,
  // no bypass-fade crossfader, no stereo linking - all of which would run
  // unconditionally per-block in the standard path and dominate per-channel
  // CPU when 16 strips are active. Master + aux buses still use the bare
  // UniversalCompressor (Bus mode + the extras earn their keep there).
  #include "ChannelComp.h"          // shared/channel-comp/ - header-only
#endif

namespace focal
{
// Phase 1a channel strip: 4K-style HPF + 4-band EQ + per-aux sends + pan +
// fader + SIP gating. FET/Opto/VCA compressor inserts after the EQ stage in
// the next chunk; the API stays the same.
class ChannelStrip
{
public:
    static constexpr int kNumBuses    = ChannelStripParams::kNumBuses;
    static constexpr int kNumAuxSends = ChannelStripParams::kNumAuxSends;

    void prepare (double sampleRate, int blockSize);
    void bind (const ChannelStripParams& params) noexcept { paramsRef = &params; }

    // Bind the per-app PluginManager so this strip's PluginSlot can resolve
    // file paths to plugin instances. Called once at engine construction
    // after AudioEngine builds its PluginManager. The slot is dormant until
    // a plugin is loaded - strips with no plugin pay zero RT cost.
    void bindPluginManager (PluginManager& mgr) noexcept { pluginSlot.setManager (mgr); }
    PluginSlot&       getPluginSlot()       noexcept { return pluginSlot; }
    const PluginSlot& getPluginSlot() const noexcept { return pluginSlot; }

    // Engine sets this to true when the recorder is going to read
    // getLastProcessedMono() this block (i.e. armed && recording && printEffects).
    // When false, the strip is allowed to skip the DSP if it isn't passing
    // to master (passByGate=false), saving the cost of a full
    // UniversalCompressor + EQ pass on every silent channel.
    void setNeedsProcessedMono (bool needed) noexcept { needsProcessedMono = needed; }

    // Latest peak gain reduction in dB (negative = reducing). Read from any
    // thread; written from the audio thread. Used by the per-channel GR
    // meter in the UI.
    float getCurrentGrDb() const noexcept { return currentGrDb.load (std::memory_order_relaxed); }

    // Pointer to the post-EQ/post-comp mono buffer from the most-recent
    // processAndAccumulate() call. Valid for `lastProcessedSamples` samples.
    // The recorder reads this when "print effects" is engaged on a track.
    // Returns nullptr if no DSP ran this block (no input or unbound params).
    const float* getLastProcessedMono() const noexcept { return lastProcessedPtr; }
    int getLastProcessedSamples() const noexcept { return lastProcessedSamples; }

    // Reads `numSamples` of mono audio from `monoIn`. Internal: HPF + 4-band EQ
    // (single source from dsp-cores), then accumulates:
    //   • post-fader stereo signal into masterL/masterR (when not bus-routed)
    //   • binary-routed signal into busL[N]/busR[N] for each assigned bus
    //   • aux-send signal into auxLaneL[N]/auxLaneR[N] at auxSendDb[N] gain,
    //     pre- or post-fader per auxSendPreFader[N]
    void processAndAccumulate (const float* monoIn,
                               float* masterL, float* masterR,
                               const std::array<float*, kNumBuses>& busL,
                               const std::array<float*, kNumBuses>& busR,
                               const std::array<float*, kNumAuxSends>& auxLaneL,
                               const std::array<float*, kNumAuxSends>& auxLaneR,
                               int numSamples,
                               bool passByGate) noexcept;

private:
    const ChannelStripParams* paramsRef = nullptr;
    juce::SmoothedValue<float> faderGain  { 0.0f };
    juce::SmoothedValue<float> panGainL   { 0.7071f };
    juce::SmoothedValue<float> panGainR   { 0.7071f };
    // Binary bus routing - smoothed 0..1 to avoid clicks on toggle.
    std::array<juce::SmoothedValue<float>, kNumBuses> busGain;
    // Aux send levels - linear gain (mapped from auxSendDb each block).
    // Smoothed so knob turns and pre/post toggles don't click.
    std::array<juce::SmoothedValue<float>, kNumAuxSends> auxSendGain;
    // Cached per-block: which aux sends use the pre-fader tap. Sampled at
    // the top of processAndAccumulate so the inner loop avoids per-sample
    // atomic loads.
    std::array<bool, kNumAuxSends> auxSendPre {};

    std::vector<float> tempMono;  // pre-fader scratch buffer (post-EQ)
    juce::AudioBuffer<float> eqMonoBuffer;  // wraps tempMono for BritishEQProcessor::process

    // Per-channel insert plugin. Sits between phase invert and the EQ stage
    // so the user can drop a guitar amp / synth / character processor in
    // the signal path before the channel's EQ + comp shape it.
    PluginSlot pluginSlot;

#if FOCAL_HAS_DUSK_DSP
    BritishEQProcessor eq;
    // Cache of the most recent Parameters we pushed to `eq`. updateEqParameters
    // memcmp's the freshly-built Parameters against this and only calls
    // setParameters() when the bytes differ - this skips the full coefficient
    // recompute (8-14 biquads) on every block when the user isn't currently
    // turning a knob. Value-init {} so padding bytes are zeroed and the
    // memcmp doesn't false-positive on garbage padding.
    BritishEQProcessor::Parameters lastEqParams {};

    // Per-channel comp - facade over UniversalCompressor with the minimal
    // processing fast path active. One instance per channel. Per-mode
    // parameter pointers are cached in bindCompParams() to avoid string
    // lookups on the audio thread.
    dusk::ChannelComp compressor;
    juce::AudioBuffer<float> compMonoBuffer;  // sized in prepare(); used only as a chunking fence

    // Cached APVTS raw atomic value pointers - set once in bindCompParams().
    // These point at the SAME std::atomic<float> that UniversalCompressor's
    // processBlock() reads via getRawParameterValue(); writing here is the
    // direct, lock-free path. Stores hold DENORMALISED (SI-unit / index /
    // 0-or-1) values - no normalisation step needed.
    std::atomic<float>* compModeAtom        = nullptr;
    std::atomic<float>* compBypassAtom      = nullptr;
    std::atomic<float>* compMixAtom         = nullptr;
    std::atomic<float>* compAutoMakeupAtom  = nullptr;
    std::atomic<float>* compOptoPeakRedAtom = nullptr;
    std::atomic<float>* compOptoGainAtom    = nullptr;
    std::atomic<float>* compOptoLimitAtom   = nullptr;
    std::atomic<float>* compFetInputAtom    = nullptr;
    std::atomic<float>* compFetOutputAtom   = nullptr;
    std::atomic<float>* compFetAttackAtom   = nullptr;
    std::atomic<float>* compFetReleaseAtom  = nullptr;
    std::atomic<float>* compFetRatioAtom    = nullptr;
    std::atomic<float>* compVcaThreshAtom   = nullptr;
    std::atomic<float>* compVcaRatioAtom    = nullptr;
    std::atomic<float>* compVcaAttackAtom   = nullptr;
    std::atomic<float>* compVcaReleaseAtom  = nullptr;
    std::atomic<float>* compVcaOutputAtom   = nullptr;

    void bindCompParams();
    static inline void storeAtom (std::atomic<float>* a, float v) noexcept
    {
        if (a != nullptr) a->store (v, std::memory_order_relaxed);
    }
#endif

    std::atomic<float> currentGrDb { 0.0f };
    const float* lastProcessedPtr = nullptr;  // updated each processAndAccumulate()
    int          lastProcessedSamples = 0;
    bool         needsProcessedMono = false;  // set per-block by the engine

    void updateGainTargets() noexcept;
    void updateEqParameters() noexcept;
    void updateCompParameters() noexcept;
};
} // namespace focal
