#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <atomic>
#include <vector>
#include "../dsp/AuxBusStrip.h"
#include "../dsp/ChannelStrip.h"
#include "../dsp/MasterBus.h"
#include "../session/Session.h"
#include "PlaybackEngine.h"
#include "RecordManager.h"
#include "Transport.h"

namespace adhdaw
{
// Phase 2 engine: input -> channel strip (live or playback source) -> aux/master.
// Adds Transport state, recording (RecordManager), and playback (PlaybackEngine).
class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    explicit AudioEngine (Session& sessionToBindTo);
    ~AudioEngine() override;

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    Transport&        getTransport()      noexcept { return transport; }
    const Transport&  getTransport() const noexcept { return transport; }
    RecordManager&    getRecordManager()   noexcept { return recordManager; }
    PlaybackEngine&   getPlaybackEngine()  noexcept { return playbackEngine; }

    // Convenience for the UI; runs on the message thread. Coordinates the
    // RecordManager / PlaybackEngine state changes around Transport.
    void play();
    void stop();
    void record();

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    double getCurrentSampleRate() const noexcept { return currentSampleRate.load (std::memory_order_relaxed); }
    int    getCurrentBlockSize() const noexcept  { return currentBlockSize.load  (std::memory_order_relaxed); }
    int    getXRunCount() const noexcept         { return xrunCount.load         (std::memory_order_relaxed); }

private:
    Session& session;
    juce::AudioDeviceManager deviceManager;

    Transport       transport;
    RecordManager   recordManager   { session };
    PlaybackEngine  playbackEngine  { session };

    std::array<ChannelStrip, Session::kNumTracks> strips;
    std::array<AuxBusStrip,  Session::kNumAuxBuses> auxStrips;
    MasterBus master;

    std::vector<float> mixL, mixR;
    std::array<std::vector<float>, Session::kNumAuxBuses> auxL, auxR;
    std::vector<float> playbackScratch;  // per-track playback read scratch

    std::atomic<double> currentSampleRate { 0.0 };
    std::atomic<int>    currentBlockSize  { 0 };
    std::atomic<int>    xrunCount         { 0 };
};
} // namespace adhdaw
