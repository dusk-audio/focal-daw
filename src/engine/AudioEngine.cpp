#include "AudioEngine.h"

namespace adhdaw
{
AudioEngine::AudioEngine()
{
    deviceManager.initialiseWithDefaultDevices (2, 2);
    deviceManager.addAudioCallback (this);
}

AudioEngine::~AudioEngine()
{
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    currentSampleRate.store (device->getCurrentSampleRate(), std::memory_order_relaxed);
    currentBlockSize.store  (device->getCurrentBufferSizeSamples(), std::memory_order_relaxed);
}

void AudioEngine::audioDeviceStopped()
{
    currentSampleRate.store (0.0, std::memory_order_relaxed);
    currentBlockSize.store  (0,   std::memory_order_relaxed);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    // Phase 1a stub: copy inputs to outputs (1:1 where channels exist; pad
    // unmatched output channels with silence). The full mixer graph replaces
    // this in the next chunk.
    for (int ch = 0; ch < numOutputChannels; ++ch)
    {
        if (auto* out = outputChannelData[ch])
        {
            if (ch < numInputChannels && inputChannelData[ch] != nullptr)
                std::memcpy (out, inputChannelData[ch], sizeof (float) * (size_t) numSamples);
            else
                std::memset (out, 0, sizeof (float) * (size_t) numSamples);
        }
    }
}
} // namespace adhdaw
