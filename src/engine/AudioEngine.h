#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>

namespace adhdaw
{
// Phase 1a stub: pass-through audio callback. Subsequent chunks add the
// channel-strip array, aux/master buses, and plugin slots.
class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    double getCurrentSampleRate() const noexcept { return currentSampleRate.load (std::memory_order_relaxed); }
    int getCurrentBlockSize() const noexcept     { return currentBlockSize.load (std::memory_order_relaxed); }

private:
    juce::AudioDeviceManager deviceManager;
    std::atomic<double> currentSampleRate { 0.0 };
    std::atomic<int>    currentBlockSize  { 0 };
};
} // namespace adhdaw
