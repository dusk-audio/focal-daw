#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <vector>
#include "../session/Session.h"

#if ADHDAW_HAS_DUSK_DSP
  #include "BritishEQProcessor.h"   // multi-q
  #include "UniversalCompressor.h"  // multi-comp (juce::AudioProcessor)
#endif

namespace adhdaw
{
// Phase 1a channel strip: 4K-style HPF + 4-band EQ + per-aux sends + pan +
// fader + SIP gating. FET/Opto/VCA compressor inserts after the EQ stage in
// the next chunk; the API stays the same.
class ChannelStrip
{
public:
    static constexpr int kNumBuses = ChannelStripParams::kNumBuses;

    void prepare (double sampleRate, int blockSize);
    void bind (const ChannelStripParams& params) noexcept { paramsRef = &params; }

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
    // (single source from dsp-cores), then accumulates the post-fader stereo
    // signal into masterL/masterR and into busL[N]/busR[N] for each assigned
    // bus (binary on/off — each bus receives the channel signal at unity).
    void processAndAccumulate (const float* monoIn,
                               float* masterL, float* masterR,
                               const std::array<float*, kNumBuses>& busL,
                               const std::array<float*, kNumBuses>& busR,
                               int numSamples,
                               bool passByGate) noexcept;

private:
    const ChannelStripParams* paramsRef = nullptr;
    juce::SmoothedValue<float> faderGain  { 0.0f };
    juce::SmoothedValue<float> panGainL   { 0.7071f };
    juce::SmoothedValue<float> panGainR   { 0.7071f };
    // Binary bus routing — smoothed 0..1 to avoid clicks on toggle.
    std::array<juce::SmoothedValue<float>, kNumBuses> busGain;

    std::vector<float> tempMono;  // pre-fader scratch buffer (post-EQ)
    juce::AudioBuffer<float> eqMonoBuffer;  // wraps tempMono for BritishEQProcessor::process

#if ADHDAW_HAS_DUSK_DSP
    BritishEQProcessor eq;

    // UniversalCompressor is a juce::AudioProcessor. We treat it as an
    // embedded plugin: prepareToPlay → set APVTS params → processBlock per
    // audio block. One instance per channel. Per-mode parameter pointers
    // are cached in `bind()` to avoid string lookups on the audio thread.
    UniversalCompressor compressor;
    juce::MidiBuffer    compMidi;        // unused but required by processBlock
    juce::AudioBuffer<float> compMonoBuffer;  // pre-allocated in prepare()

    // Cached APVTS raw atomic value pointers — set once in bindCompParams().
    // These point at the SAME std::atomic<float> that UniversalCompressor's
    // processBlock() reads via getRawParameterValue(); writing here is the
    // direct, lock-free path. Stores hold DENORMALISED (SI-unit / index /
    // 0-or-1) values — no normalisation step needed.
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
} // namespace adhdaw
