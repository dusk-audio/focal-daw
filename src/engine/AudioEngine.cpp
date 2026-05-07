#include "AudioEngine.h"
#include <cstring>

namespace focal
{
AudioEngine::AudioEngine (Session& sessionToBindTo) : session (sessionToBindTo)
{
    for (int i = 0; i < Session::kNumTracks; ++i)
    {
        strips[(size_t) i].bind (session.track (i).strip);
        // Each channel strip gets a reference to the shared PluginManager
        // so its PluginSlot can resolve plugin files. Call must happen
        // before any audio processing - the slot is bypassed until a
        // plugin is actually loaded, so binding here costs nothing.
        strips[(size_t) i].bindPluginManager (pluginManager);
    }
    for (int i = 0; i < Session::kNumBuses; ++i)
        busStrips[(size_t) i].bind (session.bus (i).strip);
    for (int i = 0; i < Session::kNumAuxLanes; ++i)
    {
        auxLaneStrips[(size_t) i].bind (session.auxLane (i).params);
        // Lanes host plugins (reverb / delay / etc.) so they need the
        // shared PluginManager to resolve files into AudioPluginInstances.
        auxLaneStrips[(size_t) i].bindPluginManager (pluginManager);
    }
    master.bind (session.master());
    masteringChain.bind (session.mastering());

    // Capture the init error string so a backend-open failure (no device,
    // exclusive-mode contention, missing libjack at runtime) doesn't fail
    // silently. Empty string == success.
    if (const auto err = deviceManager.initialiseWithDefaultDevices (16, 2);
        err.isNotEmpty())
    {
        std::fprintf (stderr,
                      "[Focal/AudioEngine] device-manager init reported: %s\n",
                      err.toRawUTF8());
    }
    deviceManager.addAudioCallback (this);
}

int AudioEngine::getBackendXRunCount() const noexcept
{
    // const_cast: AudioDeviceManager::getCurrentAudioDevice is non-const for
    // historical reasons, but the read here is benign - getXRunCount itself
    // is noexcept and just returns an internal counter on the patched JUCE
    // ALSA backend (and on JACK).
    if (auto* dev = const_cast<juce::AudioDeviceManager&> (deviceManager).getCurrentAudioDevice())
        return dev->getXRunCount();
    return 0;
}

void AudioEngine::setStage (Stage s) noexcept
{
    if (stage.load (std::memory_order_relaxed) == s) return;

    // Always stop transport on a stage change so we don't leave a recorder
    // open or a mastering player rolling into a different audio path.
    if (transport.isRecording())
        recordManager.stopRecording (transport.getPlayhead());
    transport.setState (Transport::State::Stopped);
    masteringPlayer.stop();
    playbackEngine.stopPlayback();

    stage.store (s, std::memory_order_relaxed);
}

AudioEngine::~AudioEngine()
{
    if (transport.isRecording())
        recordManager.stopRecording (transport.getPlayhead());
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

void AudioEngine::play()
{
    if (transport.isPlaying() || transport.isRecording()) return;
    playbackEngine.preparePlayback();
    transport.setState (Transport::State::Playing);
}

void AudioEngine::stop()
{
    if (transport.isStopped()) return;

    const bool wasRecording = transport.isRecording();
    transport.setState (Transport::State::Stopped);

    if (wasRecording)
    {
        recordManager.stopRecording (transport.getPlayhead());
        activeRecordStart.store (std::numeric_limits<juce::int64>::min(),
                                  std::memory_order_relaxed);
    }

    playbackEngine.stopPlayback();
    transport.setPlayhead (0);
}

void AudioEngine::record()
{
    if (transport.isRecording()) return;
    if (! session.anyTrackArmed()) return;

    const double sr = currentSampleRate.load (std::memory_order_relaxed);
    if (sr <= 0.0) return;

    // With punch enabled, the captured WAV's first audible sample is at
    // punchIn - so the recorded region's timelineStart must be punchIn,
    // not the playhead at Record-press. The audio thread skips writes
    // until the playhead reaches punchIn, so the WAV time-zero lines up.
    // If the playhead is already inside the punch window (or past it) we
    // fall back to the current playhead so we don't truncate the take.
    juce::int64 startSample = transport.getPlayhead();
    if (transport.isPunchEnabled()
        && transport.getPunchOut() > transport.getPunchIn()
        && startSample < transport.getPunchIn())
    {
        startSample = transport.getPunchIn();
    }

    if (! recordManager.startRecording (sr, startSample))
        return;

    activeRecordStart.store (startSample, std::memory_order_relaxed);

    // Count-in pre-roll: roll the playhead back one bar so the metronome
    // ticks a full bar before the take begins. Audio between the pre-roll
    // start and `startSample` is gated out of the recorder by the audio
    // callback, so the WAV's first sample still maps to `startSample`.
    if (session.countInEnabled.load (std::memory_order_relaxed))
    {
        const float bpm     = session.tempoBpm.load    (std::memory_order_relaxed);
        const int   beatsBar = session.beatsPerBar.load (std::memory_order_relaxed);
        if (bpm > 0.0f && beatsBar > 0)
        {
            const auto countInSamples =
                (juce::int64) (sr * 60.0 / (double) bpm * (double) beatsBar);
            transport.setPlayhead (startSample - countInSamples);
        }
    }

    playbackEngine.preparePlayback();  // un-armed tracks still play through
    transport.setState (Transport::State::Recording);
}

void AudioEngine::publishPluginStateForSave()
{
    // Snapshot each track's PluginSlot into its session-model strings so
    // SessionSerializer (which only sees Session) can serialise plugin
    // state alongside everything else. Empty strings = no plugin loaded.
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = session.track (t);
        auto& slot  = strips[(size_t) t].getPluginSlot();
        track.pluginDescriptionXml = slot.getDescriptionXmlForSave();
        track.pluginStateBase64    = slot.getStateBase64ForSave();
    }
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        auto& lane = session.auxLane (a);
        for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
        {
            auto& slot = auxLaneStrips[(size_t) a].getPluginSlot (s);
            lane.pluginDescriptionXml[(size_t) s] = slot.getDescriptionXmlForSave();
            lane.pluginStateBase64[(size_t) s]    = slot.getStateBase64ForSave();
        }
    }
}

void AudioEngine::publishTransportStateForSave()
{
    session.savedLoopStart    = transport.getLoopStart();
    session.savedLoopEnd      = transport.getLoopEnd();
    session.savedLoopEnabled  = transport.isLoopEnabled();
    session.savedPunchIn      = transport.getPunchIn();
    session.savedPunchOut     = transport.getPunchOut();
    session.savedPunchEnabled = transport.isPunchEnabled();
}

void AudioEngine::consumeTransportStateAfterLoad()
{
    transport.setLoopRange (session.savedLoopStart, session.savedLoopEnd);
    transport.setLoopEnabled (session.savedLoopEnabled);
    transport.setPunchRange (session.savedPunchIn, session.savedPunchOut);
    transport.setPunchEnabled (session.savedPunchEnabled);
}

void AudioEngine::consumePluginStateAfterLoad()
{
    // Mirror of publish: read the (just-deserialised) strings on each track
    // and re-instantiate the plugin in the live PluginSlot. Failures log
    // and continue rather than aborting the whole session load - a missing
    // plugin shouldn't lose the rest of the user's saved state.
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& track = session.track (t);
        auto& slot  = strips[(size_t) t].getPluginSlot();
        juce::String error;
        if (! slot.restoreFromSavedState (track.pluginDescriptionXml,
                                            track.pluginStateBase64, error))
        {
            DBG ("AudioEngine: failed to restore plugin on track " << (t + 1)
                  << ": " << error);
        }
    }
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        auto& lane = session.auxLane (a);
        for (int s = 0; s < AuxLaneParams::kMaxLanePlugins; ++s)
        {
            auto& slot = auxLaneStrips[(size_t) a].getPluginSlot (s);
            juce::String error;
            if (! slot.restoreFromSavedState (lane.pluginDescriptionXml[(size_t) s],
                                                lane.pluginStateBase64[(size_t) s], error))
            {
                DBG ("AudioEngine: failed to restore plugin on aux lane " << (a + 1)
                      << " slot " << (s + 1) << ": " << error);
            }
        }
    }
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    // Detect the silent-failure mode where a per-device ALSA name resolves
    // to 0 active output channels (PipeWire's ALSA shim does this for the
    // surround/front entries - JUCE accepts the asymmetric setup, the
    // engine writes the master mix to a non-existent destination, and the
    // user gets silence with no error). Loud stderr line + a flag the UI
    // surfaces so the failure is visible instead of mysterious.
    const int activeIn  = device->getActiveInputChannels().countNumberOfSetBits();
    const int activeOut = device->getActiveOutputChannels().countNumberOfSetBits();
    if (activeOut <= 0)
    {
        std::fprintf (stderr,
                      "[Focal/AudioEngine] WARNING: device \"%s\" (type %s) opened with "
                      "0 output channels (in=%d). Engine output will be silent. Pick a "
                      "different device - \"Default ALSA Output\" or another backend - or "
                      "stop PipeWire if you want raw ALSA on this interface.\n",
                      device->getName().toRawUTF8(),
                      deviceManager.getCurrentAudioDeviceType().toRawUTF8(),
                      activeIn);
        usableOutputs.store (false, std::memory_order_relaxed);
    }
    else
    {
        usableOutputs.store (true, std::memory_order_relaxed);
    }

    prepareForSelfTest (device->getCurrentSampleRate(),
                         device->getCurrentBufferSizeSamples());
}

void AudioEngine::prepareForSelfTest (double sr, int bs)
{
    currentSampleRate.store (sr, std::memory_order_relaxed);
    currentBlockSize.store  (bs, std::memory_order_relaxed);

    // Cache the tick->seconds reciprocal once - the xrun watchdog hits this
    // twice per callback otherwise, and highResolutionTicksToSeconds does a
    // 64-bit divide internally.
    {
        const auto tps = juce::Time::getHighResolutionTicksPerSecond();
        secondsPerTick = (tps > 0) ? 1.0 / (double) tps : 0.0;
    }

    // Read the global oversampling factor once per prepare. Per-channel
    // strips don't take an oversampling param (ChannelComp is fast-path /
    // native-rate only); aux + master propagate it to their bus comps and
    // (for master) to the tape oversampler.
    const int oxFactor = session.oversamplingFactor.load (std::memory_order_relaxed);

    for (auto& s : strips)        s.prepare (sr, bs);
    for (auto& a : busStrips)     a.prepare (sr, bs, oxFactor);
    for (auto& a : auxLaneStrips) a.prepare (sr, bs);
    master.prepare (sr, bs, oxFactor);
    masteringChain.prepare (sr, bs);
    masteringPlayer.prepare (bs);
    metronome.prepare (sr);
    playbackEngine.prepare (bs);  // size the playback read scratch - audio thread mustn't allocate

    mixL.assign ((size_t) bs, 0.0f);
    mixR.assign ((size_t) bs, 0.0f);
    for (auto& v : busL)      v.assign ((size_t) bs, 0.0f);
    for (auto& v : busR)      v.assign ((size_t) bs, 0.0f);
    for (auto& v : auxLaneL)  v.assign ((size_t) bs, 0.0f);
    for (auto& v : auxLaneR)  v.assign ((size_t) bs, 0.0f);
    playbackScratch.assign ((size_t) bs, 0.0f);
}

void AudioEngine::audioDeviceStopped()
{
    currentSampleRate.store (0.0, std::memory_order_relaxed);
    currentBlockSize.store  (0,   std::memory_order_relaxed);
    // Reset to the optimistic default so a transient "no device open" state
    // doesn't stick a NO OUTPUTS warning on the UI.
    usableOutputs.store (true, std::memory_order_relaxed);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    const auto callbackStart = juce::Time::getHighResolutionTicks();

    if ((int) mixL.size() < numSamples)
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (auto* out = outputChannelData[ch])
                std::memset (out, 0, sizeof (float) * (size_t) numSamples);
        return;
    }

    // Mastering stage runs an entirely different signal flow: stereo file →
    // MasteringChain → output. No track processing, no aux/master, no
    // recorder. We share `mixL/mixR` as scratch so we don't allocate on
    // the audio thread.
    if (stage.load (std::memory_order_relaxed) == Stage::Mastering)
    {
        masteringPlayer.process (mixL.data(), mixR.data(), numSamples);
        masteringChain.processInPlace (mixL.data(), mixR.data(), numSamples);

        if (numOutputChannels >= 1 && outputChannelData[0] != nullptr)
            std::memcpy (outputChannelData[0], mixL.data(), sizeof (float) * (size_t) numSamples);
        if (numOutputChannels >= 2 && outputChannelData[1] != nullptr)
            std::memcpy (outputChannelData[1], mixR.data(), sizeof (float) * (size_t) numSamples);
        for (int ch = 2; ch < numOutputChannels; ++ch)
            if (auto* out = outputChannelData[ch])
                std::memset (out, 0, sizeof (float) * (size_t) numSamples);

        const auto sr = currentSampleRate.load (std::memory_order_relaxed);
        if (sr > 0.0)
        {
            const double bufferMs = 1000.0 * (double) numSamples / sr;
            // Multiply by cached reciprocal instead of calling
            // highResolutionTicksToSeconds (which does an internal divide
            // every call). secondsPerTick is set in prepareForSelfTest.
            const double elapsedMs = (double) (juce::Time::getHighResolutionTicks() - callbackStart)
                                         * secondsPerTick * 1000.0;
            if (elapsedMs > bufferMs)
                xrunCount.fetch_add (1, std::memory_order_relaxed);
        }
        return;
    }

    // SIMD-friendly clear via JUCE's FloatVectorOperations - it dispatches to
    // the platform's vector zero-store on x86 (SSE) / aarch64 (NEON) where
    // memset would still call into libc and miss the alignment-aware path.
    juce::FloatVectorOperations::clear (mixL.data(), numSamples);
    juce::FloatVectorOperations::clear (mixR.data(), numSamples);
    for (auto& v : busL) juce::FloatVectorOperations::clear (v.data(), numSamples);
    for (auto& v : busR) juce::FloatVectorOperations::clear (v.data(), numSamples);
    for (auto& v : auxLaneL) juce::FloatVectorOperations::clear (v.data(), numSamples);
    for (auto& v : auxLaneR) juce::FloatVectorOperations::clear (v.data(), numSamples);

    const bool anyChannelSolo = session.anyTrackSoloed();
    const bool anyBusSolo     = session.anyBusSoloed();

    std::array<float*, ChannelStrip::kNumBuses> busLPtrs {};
    std::array<float*, ChannelStrip::kNumBuses> busRPtrs {};
    for (int a = 0; a < Session::kNumBuses; ++a)
    {
        busLPtrs[(size_t) a] = busL[(size_t) a].data();
        busRPtrs[(size_t) a] = busR[(size_t) a].data();
    }
    std::array<float*, ChannelStripParams::kNumAuxSends> auxLanePtrsL {};
    std::array<float*, ChannelStripParams::kNumAuxSends> auxLanePtrsR {};
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        auxLanePtrsL[(size_t) a] = auxLaneL[(size_t) a].data();
        auxLanePtrsR[(size_t) a] = auxLaneR[(size_t) a].data();
    }

    const auto state = transport.getState();
    const bool isPlaying   = (state == Transport::State::Playing);
    const bool isRecording = (state == Transport::State::Recording);
    const juce::int64 blockStartSamples = transport.getPlayhead();

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& trackParams = session.track (t).strip;

        // Automation routing runs FIRST so the per-strip routing decisions
        // below (passes / monitorPasses / etc.) see the automated values
        // when in Read or Touch mode. Writes liveFaderDb / livePan /
        // liveAuxSendDb / liveMute. ChannelStrip's processAndAccumulate
        // also reads these live atoms when computing per-sample gains.
        //
        // The mode atom is loaded with `acquire` so the audio thread sees
        // every prior write to the lane's points vector (made on the
        // message thread during a Write pass) before reading it - the UI
        // release-stores the new mode after appending, so the load-acquire
        // here pairs with the store-release there.
        {
            const int amode = session.track (t).automationMode.load (std::memory_order_acquire);

            // Per-param continuous routing: Read pulls from the lane
            // unconditionally; Touch pulls from the lane only while the
            // user is NOT grabbing that specific control; Off / Write
            // always pass the manual setpoint through. Per-control
            // `touched` flags so a grab on the fader doesn't release pan.
            auto routeContinuous = [&] (AutomationParam param,
                                          const std::atomic<float>& manual,
                                          const std::atomic<bool>* touched,
                                          std::atomic<float>& live)
            {
                const auto& lane = session.track (t).automationLanes[(size_t) param];
                const bool readsLane =
                       amode == (int) AutomationMode::Read
                    || (amode == (int) AutomationMode::Touch
                        && touched != nullptr
                        && ! touched->load (std::memory_order_acquire));
                const float v = (readsLane && ! lane.points.empty())
                    ? evaluateLane (lane, blockStartSamples, param)
                    : manual.load (std::memory_order_relaxed);
                live.store (v, std::memory_order_relaxed);
            };

            routeContinuous (AutomationParam::FaderDb,
                              trackParams.faderDb, &trackParams.faderTouched,
                              trackParams.liveFaderDb);
            routeContinuous (AutomationParam::Pan,
                              trackParams.pan, &trackParams.panTouched,
                              trackParams.livePan);
            for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
            {
                const auto param = (AutomationParam) ((int) AutomationParam::AuxSend1 + i);
                routeContinuous (param,
                                  trackParams.auxSendDb[(size_t) i],
                                  &trackParams.auxSendTouched[(size_t) i],
                                  trackParams.liveAuxSendDb[(size_t) i]);
            }

            // Mute - discrete, no Touch flag. Read or Touch reads lane;
            // Off or Write reads manual. Discrete params return 0.0 or
            // 1.0 from evaluateLane (after denormalize); we threshold at
            // 0.5 to a bool. Empty lane falls through to manual.
            {
                const auto& lane = session.track (t).automationLanes[(size_t) AutomationParam::Mute];
                const bool readsLane =
                       (amode == (int) AutomationMode::Read
                     || amode == (int) AutomationMode::Touch);
                const bool effMute = (readsLane && ! lane.points.empty())
                    ? (evaluateLane (lane, blockStartSamples, AutomationParam::Mute) >= 0.5f)
                    : trackParams.mute.load (std::memory_order_relaxed);
                trackParams.liveMute.store (effMute, std::memory_order_relaxed);
            }
        }

        // Reads liveMute - just-routed by the block above, so the strip's
        // passes / monitorPasses calculation sees automated state.
        const bool muted   = trackParams.liveMute.load (std::memory_order_relaxed);
        const bool soloed  = trackParams.solo.load (std::memory_order_relaxed);
        const bool armed   = session.track (t).recordArmed.load (std::memory_order_relaxed);
        const bool monitorEnabled = session.track (t).inputMonitor.load (std::memory_order_relaxed);

        // The track will read from disk during Play, or during Record on
        // un-armed tracks (so other tracks keep playing while we record into
        // an armed one). Otherwise the strip's source is live device input.
        const bool willReadFromDisk = isPlaying || (isRecording && ! armed);

        // The IN button (inputMonitor) gates LIVE input from passing through
        // the strip to master - engineer turns IN off when monitoring direct
        // from the hardware interface. It must NOT gate disk playback: when
        // we're playing back a previous take (regardless of arm state), the
        // disk audio should always reach master. So monitorPasses is forced
        // true when reading from disk; only when we're routing live input
        // does the IN toggle actually silence the master path.
        const bool monitorPasses = willReadFromDisk
            ? true
            : (armed ? monitorEnabled : true);
        const bool passes = ! muted && (anyChannelSolo ? soloed : true) && monitorPasses;

        // Resolve the input source for this track.
        const int inputIdx = session.resolveInputForTrack (t);
        const float* deviceInput = (inputIdx >= 0 && inputIdx < numInputChannels)
                                    ? inputChannelData[(size_t) inputIdx] : nullptr;

        // Choose the source the channel strip will process.
        //   - Playing & not armed: read previous take from disk (un-armed
        //     playback tracks).
        //   - Playing & armed: ALSO read previous take from disk. Standard
        //     DAW behavior - stopping a record pass leaves the track armed
        //     for the next take, but Play should still reproduce what was
        //     recorded. Without this, hitting Play after a stop produced
        //     silence on the just-recorded track (the live input branch
        //     fired instead of disk read).
        //   - Recording & armed: live input feeds the strip; the recorder
        //     captures via writeInputBlock below. The IN toggle gates
        //     master accumulation (passByGate / monitorPasses), not whether
        //     the strip processes.
        //   - Stopped & armed (or stopped + IN on, un-armed): live input
        //     for monitoring.
        //   - Stopped & un-armed & IN off: monoIn null → strip is silent.
        const float* monoIn = nullptr;

        if (willReadFromDisk)
        {
            playbackEngine.readForTrack (t, blockStartSamples,
                                          playbackScratch.data(), numSamples);
            monoIn = playbackScratch.data();
        }
        else if (deviceInput != nullptr && (armed || monitorEnabled))
        {
            monoIn = deviceInput;
        }

        // Input meter - armed tracks always show live input (so the user can
        // set gain for tracking even with monitor muted). Un-armed tracks
        // show their playback signal during transport, or live input when IN
        // is engaged. With IN off and no transport, the meter reads silence
        // even though the strip's DSP is still running on deviceInput (so a
        // PRINT-mode recorder can capture post-effects audio).
        const float* meterSrc = nullptr;
        if (armed && deviceInput != nullptr)
            meterSrc = deviceInput;
        else if ((isPlaying || isRecording) && ! armed)
            meterSrc = monoIn;
        else if (monitorEnabled && deviceInput != nullptr)
            meterSrc = deviceInput;

        // SIMD'd absolute-peak via findMinAndMax then |min| vs max - one
        // vector pass over the buffer instead of a scalar abs-and-compare
        // loop per sample. Same numeric result, much faster on long blocks.
        float inputPeak = 0.0f;
        if (meterSrc != nullptr && numSamples > 0)
        {
            const auto rng = juce::FloatVectorOperations::findMinAndMax (meterSrc, numSamples);
            inputPeak = juce::jmax (std::abs (rng.getStart()), std::abs (rng.getEnd()));
        }
        const float inputDb = (inputPeak > 1e-5f)
                              ? juce::Decibels::gainToDecibels (inputPeak, -100.0f)
                              : -100.0f;
        session.track (t).meterInputDb.store (inputDb, std::memory_order_relaxed);

        // R-channel input meter for stereo tracks. Phase 1 only renders the
        // peak; the strip's audio path is still mono until Phase 2 wires in
        // stereo recording. Mono / Midi tracks get -100 so the UI can fall
        // back to a single LED bar without gating on mode.
        float inputRDb = -100.0f;
        const int rIdx = session.resolveInputRForTrack (t);
        if (rIdx >= 0 && rIdx < numInputChannels)
        {
            const float* deviceInputR = inputChannelData[(size_t) rIdx];
            if (deviceInputR != nullptr && (armed || monitorEnabled) && numSamples > 0)
            {
                const auto rng = juce::FloatVectorOperations::findMinAndMax (deviceInputR, numSamples);
                const float peak = juce::jmax (std::abs (rng.getStart()), std::abs (rng.getEnd()));
                if (peak > 1e-5f)
                    inputRDb = juce::Decibels::gainToDecibels (peak, -100.0f);
            }
        }
        session.track (t).meterInputRDb.store (inputRDb, std::memory_order_relaxed);

        // Tell the strip whether the recorder is going to ask for the
        // post-effects buffer this block - if so, the strip MUST run its
        // DSP even when it's not passing to master. Otherwise we let it
        // skip the heavy pass to save CPU on silent tracks.
        const bool needPrintBuffer = isRecording && armed && deviceInput != nullptr
                                  && session.track (t).printEffects.load (std::memory_order_relaxed);
        strips[(size_t) t].setNeedsProcessedMono (needPrintBuffer);

        strips[(size_t) t].processAndAccumulate (monoIn,
                                                 mixL.data(), mixR.data(),
                                                 busLPtrs, busRPtrs,
                                                 auxLanePtrsL, auxLanePtrsR,
                                                 numSamples,
                                                 passes && monoIn != nullptr);
        session.track (t).meterGrDb.store (strips[(size_t) t].getCurrentGrDb(),
                                            std::memory_order_relaxed);

        // Recording capture - armed tracks always commit their input to
        // disk while recording. By default the raw deviceInput is written;
        // when `printEffects` is engaged, the post-EQ/post-comp buffer that
        // the strip just produced is written instead, "printing" the
        // channel-strip processing to the WAV.
        //
        // Punch: when transport.isPunchEnabled(), only samples whose timeline
        // position lies inside [punchIn, punchOut) are committed. The block
        // is sliced to the intersection of [blockStart, blockEnd) and the
        // punch window; outside samples are silently discarded (NOT written
        // as zeros, so the WAV's frame count equals the punch window length).
        if (isRecording && armed && deviceInput != nullptr)
        {
            const bool printEfx = session.track (t).printEffects.load (std::memory_order_relaxed);
            const float* recSrc = deviceInput;
            if (printEfx)
            {
                if (auto* processed = strips[(size_t) t].getLastProcessedMono();
                    processed != nullptr
                    && strips[(size_t) t].getLastProcessedSamples() >= numSamples)
                {
                    recSrc = processed;
                }
            }

            // Unified write-gate - honours BOTH:
            //   • activeRecordStart  (count-in pre-roll: skip writes until
            //     the playhead reaches the take's intended start)
            //   • punch-in / punch-out  (only commit samples in the window
            //     when punch is on)
            // Both reduce to a single [from, to) intersection with the
            // current block.
            const auto recStart  = activeRecordStart.load (std::memory_order_relaxed);
            juce::int64 effIn    = recStart;  // floor; never write before this
            juce::int64 effOut   = std::numeric_limits<juce::int64>::max();
            if (transport.isPunchEnabled())
            {
                const auto pIn  = transport.getPunchIn();
                const auto pOut = transport.getPunchOut();
                if (pOut > pIn)
                {
                    effIn  = juce::jmax (effIn, pIn);
                    effOut = pOut;
                }
                else
                {
                    effIn = effOut;  // empty/inverted punch window → no capture
                }
            }

            const auto blockEnd = blockStartSamples + numSamples;
            const auto sliceStart = juce::jmax (blockStartSamples, effIn);
            const auto sliceEnd   = juce::jmin (blockEnd,        effOut);
            if (sliceEnd > sliceStart)
            {
                const int writeOffset = (int) (sliceStart - blockStartSamples);
                const int writeLength = (int) (sliceEnd - sliceStart);
                recordManager.writeInputBlock (t, recSrc + writeOffset, writeLength);
            }
        }
    }

    for (int a = 0; a < Session::kNumBuses; ++a)
    {
        const auto& params = session.bus (a).strip;
        const bool muted   = params.mute.load (std::memory_order_relaxed);
        const bool soloed  = params.solo.load (std::memory_order_relaxed);
        const bool passes  = ! muted && (anyBusSolo ? soloed : true);

        if (! passes) continue;

        // Skip aux DSP entirely when the bus buffer is silent - no channel
        // routed to it AND no smoothing tail from a recently-unassigned
        // channel. UniversalCompressor (Bus mode) is one of the heaviest
        // per-block ops in the engine; skipping the whole EQ + comp pass on
        // an idle aux is a big saving when only 1-2 of the 4 buses are in
        // use. Aux's own fader/pan smoothers don't tick during the skip,
        // but they pick up correctly from their last-known current value
        // when audio resumes, so no click on re-engage.
        {
            const auto rngL = juce::FloatVectorOperations::findMinAndMax (
                                  busL[(size_t) a].data(), numSamples);
            const auto rngR = juce::FloatVectorOperations::findMinAndMax (
                                  busR[(size_t) a].data(), numSamples);
            const float peak = juce::jmax (
                juce::jmax (std::abs (rngL.getStart()), std::abs (rngL.getEnd())),
                juce::jmax (std::abs (rngR.getStart()), std::abs (rngR.getEnd())));
            if (peak <= 1e-6f) continue;
        }

        busStrips[(size_t) a].processInPlace (busL[(size_t) a].data(),
                                              busR[(size_t) a].data(),
                                              numSamples);
        // SIMD'd mix accumulate - hot inner loop, runs once per active aux
        // per callback. JUCE picks the right SSE/NEON path based on the
        // platform; cheaper than scalar [i]+= even with -O3.
        juce::FloatVectorOperations::add (mixL.data(),
                                            busL[(size_t) a].data(),
                                            numSamples);
        juce::FloatVectorOperations::add (mixR.data(),
                                            busR[(size_t) a].data(),
                                            numSamples);
    }

    // AUX return lanes - process each lane's accumulated send buffer
    // through its plugin chain, then sum the wet output into master. Same
    // silence-skip optimisation as the bus pass above so idle lanes (no
    // channel sending to them) don't run their plugins.
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
    {
        {
            const auto rngL = juce::FloatVectorOperations::findMinAndMax (
                                  auxLaneL[(size_t) a].data(), numSamples);
            const auto rngR = juce::FloatVectorOperations::findMinAndMax (
                                  auxLaneR[(size_t) a].data(), numSamples);
            const float peak = juce::jmax (
                juce::jmax (std::abs (rngL.getStart()), std::abs (rngL.getEnd())),
                juce::jmax (std::abs (rngR.getStart()), std::abs (rngR.getEnd())));
            if (peak <= 1e-6f) continue;
        }

        auxLaneStrips[(size_t) a].processStereoBlock (auxLaneL[(size_t) a].data(),
                                                        auxLaneR[(size_t) a].data(),
                                                        numSamples);
        juce::FloatVectorOperations::add (mixL.data(),
                                            auxLaneL[(size_t) a].data(),
                                            numSamples);
        juce::FloatVectorOperations::add (mixR.data(),
                                            auxLaneR[(size_t) a].data(),
                                            numSamples);
    }

    master.processInPlace (mixL.data(), mixR.data(), numSamples);

    // Metronome - push session BPM + enable into the click generator each
    // block (cheap atomic loads), then mix the click into the post-master
    // bus. Click is post-master so it never gets EQ'd or compressed by
    // the master strip - it's purely a monitoring aid.
    metronome.setEnabled    (session.metronomeEnabled.load (std::memory_order_relaxed));
    metronome.setBpm        (session.tempoBpm.load (std::memory_order_relaxed));
    metronome.setBeatsPerBar(session.beatsPerBar.load (std::memory_order_relaxed));
    metronome.setVolumeDb   (session.metronomeVolDb.load (std::memory_order_relaxed));
    {
        // Force the click on during count-in pre-roll so the user always
        // gets the count-in even if they haven't engaged CLICK manually.
        const auto recStart   = activeRecordStart.load (std::memory_order_relaxed);
        const bool inCountIn  = isRecording && blockStartSamples < recStart;
        metronome.process (blockStartSamples, isPlaying || isRecording,
                            mixL.data(), mixR.data(), numSamples,
                            /*forceEnable*/ inCountIn);
    }

    if (numOutputChannels >= 1 && outputChannelData[0] != nullptr)
        std::memcpy (outputChannelData[0], mixL.data(), sizeof (float) * (size_t) numSamples);
    if (numOutputChannels >= 2 && outputChannelData[1] != nullptr)
        std::memcpy (outputChannelData[1], mixR.data(), sizeof (float) * (size_t) numSamples);
    for (int ch = 2; ch < numOutputChannels; ++ch)
        if (auto* out = outputChannelData[ch])
            std::memset (out, 0, sizeof (float) * (size_t) numSamples);

    if (isPlaying || isRecording)
    {
        transport.advancePlayhead (numSamples);

        // Loop wrap-around. Only honoured during plain playback - during
        // Recording we keep the playhead linear so the captured WAV maps
        // cleanly onto the timeline (loop-take-stacking is a future
        // feature). Wrap is whole-block accurate: we do not split the
        // current block, so the playhead may briefly read up to one block
        // past loopEnd before snapping back. That overshoot is silent
        // because PlaybackEngine returns silence outside region bounds.
        if (isPlaying && ! isRecording && transport.isLoopEnabled())
        {
            const auto lStart = transport.getLoopStart();
            const auto lEnd   = transport.getLoopEnd();
            if (lEnd > lStart)
            {
                const auto curr = transport.getPlayhead();
                if (curr >= lEnd)
                {
                    const auto loopLen   = lEnd - lStart;
                    const auto overshoot = (curr - lEnd) % loopLen;
                    transport.setPlayhead (lStart + overshoot);
                }
            }
        }
    }

    // Detect xrun: callback work shouldn't exceed the buffer's wall-clock
    // budget. If it does, we'd glitch on the next callback. Track the count
    // for the status bar.
    const auto sr = currentSampleRate.load (std::memory_order_relaxed);
    if (sr > 0.0)
    {
        const double bufferMs = 1000.0 * (double) numSamples / sr;
        const double elapsedMs = (double) (juce::Time::getHighResolutionTicks() - callbackStart)
                                     * secondsPerTick * 1000.0;
        if (elapsedMs > bufferMs)
            xrunCount.fetch_add (1, std::memory_order_relaxed);
    }
}
} // namespace focal
