#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <atomic>
#include <vector>
#include "../dsp/AuxBusStrip.h"
#include "../dsp/ChannelStrip.h"
#include "../dsp/MasterBus.h"
#include "../dsp/MasteringChain.h"
#include "../dsp/Metronome.h"
#include "../session/Session.h"
#include "MasteringPlayer.h"
#include "PlaybackEngine.h"
#include "PluginManager.h"
#include "RecordManager.h"
#include "Transport.h"

namespace adhdaw
{
// Phase 2 engine: input -> channel strip (live or playback source) -> aux/master.
// Adds Transport state, recording (RecordManager), and playback (PlaybackEngine).
class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    // Workflow stages - drives which signal flow the audio callback runs.
    // Recording / Mixing share the live track-to-master path (input + disk
    // playback into channel strips → aux / master); Mastering swaps that
    // for stereo file → MasteringChain → output.
    enum class Stage { Recording, Mixing, Mastering };

    explicit AudioEngine (Session& sessionToBindTo);
    ~AudioEngine() override;

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    Session&          getSession()        noexcept { return session; }
    const Session&    getSession() const   noexcept { return session; }
    Transport&        getTransport()      noexcept { return transport; }
    const Transport&  getTransport() const noexcept { return transport; }
    RecordManager&    getRecordManager()   noexcept { return recordManager; }
    PlaybackEngine&   getPlaybackEngine()  noexcept { return playbackEngine; }
    PluginManager&    getPluginManager()   noexcept { return pluginManager; }
    MasteringPlayer&  getMasteringPlayer() noexcept { return masteringPlayer; }
    MasteringChain&   getMasteringChain()  noexcept { return masteringChain; }
    Metronome&        getMetronome()       noexcept { return metronome; }

    Stage getStage() const noexcept { return stage.load (std::memory_order_relaxed); }
    void  setStage (Stage s) noexcept;

    // Single per-app UndoManager. Owns the stack of UndoableActions for
    // region edits and any other undoable mutation we add later. Touched
    // only on the message thread. It's a ChangeBroadcaster - TapeStrip
    // registers to repaint when the stack moves.
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }

    // App-lifetime region clipboard. Empty `region.file` means nothing
    // to paste. Only one slot - multi-region or cross-track multi-select
    // are post-MVP.
    struct RegionClipboard
    {
        bool        hasContent = false;
        int         sourceTrack = -1;
        AudioRegion region;
    };
    RegionClipboard& getRegionClipboard() noexcept { return regionClipboard; }

    // Per-strip access - UI components reach into the engine to call
    // PluginSlot::loadFromFile etc. on the message thread. The audio path
    // still owns the strips; this is just a typed accessor.
    ChannelStrip&       getStrip (int idx)       noexcept { return strips[(size_t) idx]; }
    const ChannelStrip& getStrip (int idx) const noexcept { return strips[(size_t) idx]; }

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

    // Self-test entry: prepare engine state (strip/aux/master DSP, mix
    // buffers) for a given sample rate and block size WITHOUT a real
    // AudioIODevice. The pipeline self-test detaches the engine from
    // deviceManager and calls audioDeviceIOCallbackWithContext directly with
    // synthetic input/output buffers. After the test, prepareForSelfTest can
    // be called again with the live device's params (or audioDeviceAboutToStart
    // will be invoked when the engine is re-attached).
    void prepareForSelfTest (double sampleRate, int blockSize);

    // Plugin-slot persistence - message-thread bookends around
    // SessionSerializer::save / ::load. publish copies each PluginSlot's
    // current description + state into its Track's pluginDescriptionXml /
    // pluginStateBase64 fields so the serializer can pick them up.
    // consume reads those Track fields back and asks each PluginSlot to
    // re-create + restore.
    void publishPluginStateForSave();
    void consumePluginStateAfterLoad();

    // Transport (loop + punch) persistence - same publish/consume bookend.
    // The atomics live on Transport for audio-thread access; the serializer
    // sees only Session, so we mirror them onto Session.savedLoop* /
    // savedPunch* before save and apply them back after load.
    void publishTransportStateForSave();
    void consumeTransportStateAfterLoad();

    double getCurrentSampleRate() const noexcept { return currentSampleRate.load (std::memory_order_relaxed); }
    int    getCurrentBlockSize() const noexcept  { return currentBlockSize.load  (std::memory_order_relaxed); }

    // Engine-side xrun count: callbacks whose wall-clock time exceeded the
    // buffer's audio time. Detected at the end of each callback in
    // audioDeviceIOCallbackWithContext.
    int    getXRunCount() const noexcept         { return xrunCount.load         (std::memory_order_relaxed); }

    // Backend-side xrun count (e.g. ALSA snd_pcm_recover EPIPE events). Read
    // from AudioIODevice::getXRunCount on the message thread; returns 0 if no
    // device is currently open. Independent from the engine-side counter
    // above - backend xruns happen when the kernel/driver couldn't hand us a
    // buffer in time, even if our callback would have been fast.
    int    getBackendXRunCount() const noexcept;

    // True when the currently-open audio device has at least one active
    // output channel. False is the failure mode where a per-device ALSA name
    // (e.g. "UMC1820, USB Audio; Front output / input") is selected while
    // PipeWire is running - JUCE/PipeWire opens it with 0 output channels,
    // the engine writes its master mix to a buffer with no destinations, and
    // the user gets silent output with no error. Detected in
    // audioDeviceAboutToStart and surfaced by SystemStatusBar.
    bool   hasUsableOutputs() const noexcept { return usableOutputs.load (std::memory_order_relaxed); }

private:
    Session& session;
    juce::AudioDeviceManager deviceManager;

    Transport       transport;
    RecordManager   recordManager   { session };
    PlaybackEngine  playbackEngine  { session };
    PluginManager   pluginManager;  // shared across all per-channel PluginSlots
    juce::UndoManager undoManager;  // region edits + any future undoable mutation
    RegionClipboard   regionClipboard;

    MasteringPlayer  masteringPlayer;
    MasteringChain   masteringChain;
    Metronome        metronome;
    std::atomic<Stage> stage { Stage::Mixing };

    std::array<ChannelStrip, Session::kNumTracks> strips;
    std::array<AuxBusStrip,  Session::kNumAuxBuses> auxStrips;
    MasterBus master;

    std::vector<float> mixL, mixR;
    std::array<std::vector<float>, Session::kNumAuxBuses> auxL, auxR;
    std::vector<float> playbackScratch;  // per-track playback read scratch

    std::atomic<double> currentSampleRate { 0.0 };
    std::atomic<int>    currentBlockSize  { 0 };
    std::atomic<int>    xrunCount         { 0 };

    // Cached 1.0 / juce::Time::getHighResolutionTicksPerSecond(). The
    // tick->seconds conversion runs twice per audio callback (xrun watchdog
    // entry + exit). highResolutionTicksToSeconds() does an internal divide
    // every call; multiplying against this cached reciprocal avoids both
    // divides on the audio thread. Set in prepareForSelfTest /
    // audioDeviceAboutToStart, never written from the audio thread.
    double secondsPerTick { 0.0 };

    // True until proven false in audioDeviceAboutToStart (default true so
    // SystemStatusBar shows nothing alarming pre-open).
    std::atomic<bool>   usableOutputs     { true };

    // Where the active record session's WAV starts on the timeline (= the
    // first sample committed to disk). The audio callback uses this to
    // gate writes - under count-in the playhead is rolled back BEFORE this
    // sample so the metronome can tick a pre-roll, and writes are skipped
    // until the playhead catches up. INT64_MIN sentinel = no record active.
    std::atomic<juce::int64> activeRecordStart { std::numeric_limits<juce::int64>::min() };
};
} // namespace adhdaw
