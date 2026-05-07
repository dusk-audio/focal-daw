#include "AuxLaneStrip.h"
#include <cmath>

namespace focal
{
void AuxLaneStrip::prepare (double sampleRate, int blockSize)
{
    returnGain.reset (sampleRate, 0.020);
    returnGain.setCurrentAndTargetValue (1.0f);
    for (auto& s : slots)
        s.prepareToPlay (sampleRate, juce::jmax (1, blockSize));
}

void AuxLaneStrip::updateGainTarget() noexcept
{
    if (paramsRef == nullptr) return;
    const float db = paramsRef->returnLevelDb.load (std::memory_order_relaxed);
    // Same -inf-via-sentinel pattern as the channel strip's fader: anything
    // at or below the floor hard-mutes (avoids feeding reverb tails through
    // an inaudible-but-nonzero gain).
    const float gain = (db <= ChannelStripParams::kFaderInfThreshDb)
                         ? 0.0f
                         : juce::Decibels::decibelsToGain (db);
    returnGain.setTargetValue (gain);
}

void AuxLaneStrip::processStereoBlock (float* L, float* R, int numSamples) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    if (numSamples == 0) return;
    if (paramsRef == nullptr) return;

    // Mute path: clear in-place so the engine's accumulator into master
    // sees silence (the lane's buffer is reused across blocks).
    if (paramsRef->mute.load (std::memory_order_relaxed))
    {
        std::memset (L, 0, sizeof (float) * (size_t) numSamples);
        std::memset (R, 0, sizeof (float) * (size_t) numSamples);
        paramsRef->meterPostL.store (-100.0f, std::memory_order_relaxed);
        paramsRef->meterPostR.store (-100.0f, std::memory_order_relaxed);
        return;
    }

    updateGainTarget();

    // Plugins in series. Each slot is a no-op when nothing's loaded; the
    // time-budget watchdog inside PluginSlot auto-bypasses on stalls.
    for (auto& s : slots)
        s.processStereoBlock (L, R, numSamples);

    // Return level + meter peak.
    float postPeakL = 0.0f, postPeakR = 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
        const float g = returnGain.getNextValue();
        L[i] *= g;
        R[i] *= g;
        const float aL = std::fabs (L[i]);
        const float aR = std::fabs (R[i]);
        if (aL > postPeakL) postPeakL = aL;
        if (aR > postPeakR) postPeakR = aR;
    }

    const auto toDb = [] (float a)
    {
        return a > 1.0e-5f ? juce::Decibels::gainToDecibels (a, -100.0f) : -100.0f;
    };
    paramsRef->meterPostL.store (toDb (postPeakL), std::memory_order_relaxed);
    paramsRef->meterPostR.store (toDb (postPeakR), std::memory_order_relaxed);
}
} // namespace focal
