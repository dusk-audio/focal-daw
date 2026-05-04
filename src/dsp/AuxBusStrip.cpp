#include "AuxBusStrip.h"

namespace adhdaw
{
void AuxBusStrip::prepare (double sampleRate, int /*blockSize*/)
{
    faderGain.reset (sampleRate, 0.020);
    faderGain.setCurrentAndTargetValue (1.0f);
}

void AuxBusStrip::processInPlace (float* L, float* R, int numSamples) noexcept
{
    if (paramsRef != nullptr)
    {
        const float faderDb = paramsRef->faderDb.load (std::memory_order_relaxed);
        const float gain = (faderDb <= ChannelStripParams::kFaderInfThreshDb)
                           ? 0.0f
                           : juce::Decibels::decibelsToGain (faderDb);
        faderGain.setTargetValue (gain);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        const float g = faderGain.getNextValue();
        L[i] *= g;
        R[i] *= g;
    }
}
} // namespace adhdaw
