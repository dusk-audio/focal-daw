#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include "../session/Session.h"
#include "../engine/PluginSlot.h"

namespace focal
{
// AUX return lane DSP. Hosts up to AuxLaneParams::kMaxLanePlugins plugin
// slots in series, applies a return-level gain, and writes stereo output
// meters. The lane's input buffer is the per-channel send accumulation done
// in ChannelStrip::processAndAccumulate; this class processes that buffer
// in place and the AudioEngine sums it into master after the bus pass.
//
// AuxLaneStrip is structurally lighter than BusStrip (no EQ, no comp, no
// pan): the EQ/comp role on a return is the plugin's job - typical FX
// returns are a reverb or a delay, not a bus subgroup.
class AuxLaneStrip
{
public:
    static constexpr int kMaxPlugins = AuxLaneParams::kMaxLanePlugins;

    AuxLaneStrip() = default;

    void prepare (double sampleRate, int blockSize);
    void bind (const AuxLaneParams& params) noexcept
    {
        paramsRef = &params;
        // Seed the smoother to the actual return level so the first block
        // doesn't ramp from unity → bound value (audible level jump on
        // session load when a lane was saved at e.g. -10 dB).
        const float db = params.returnLevelDb.load (std::memory_order_relaxed);
        const float gain = (db <= ChannelStripParams::kFaderInfThreshDb)
                             ? 0.0f
                             : juce::Decibels::decibelsToGain (db);
        returnGain.setCurrentAndTargetValue (gain);
    }

    // Bind the per-app PluginManager so this lane's slots can resolve plugin
    // files. Mirrors the channel-strip binding done at engine construction.
    void bindPluginManager (PluginManager& mgr) noexcept
    {
        for (auto& s : slots) s.setManager (mgr);
    }

    PluginSlot&       getPluginSlot (int idx)       noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return slots[(size_t) idx]; }
    const PluginSlot& getPluginSlot (int idx) const noexcept { jassert (idx >= 0 && idx < kMaxPlugins); return slots[(size_t) idx]; }

    // Audio-thread API. Buffer holds the summed channel sends (already
    // pan-aware stereo). Runs each loaded plugin in series, applies the
    // return-level fader, writes meter atoms. When the lane is muted, the
    // buffer is cleared and processing is skipped.
    void processStereoBlock (float* L, float* R, int numSamples) noexcept;

private:
    const AuxLaneParams* paramsRef = nullptr;
    juce::SmoothedValue<float> returnGain { 1.0f };

    std::array<PluginSlot, kMaxPlugins> slots;

    // Empty MIDI buffer threaded through the slots' processStereoBlock calls.
    // Aux lanes host effect plugins only, so this stays empty - kept as a
    // member purely so the audio thread doesn't default-construct one.
    juce::MidiBuffer pluginMidiScratch;

    void updateGainTarget() noexcept;
};
} // namespace focal
