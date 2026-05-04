#include "AudioEngine.h"
#include <cstring>

namespace adhdaw
{
AudioEngine::AudioEngine (Session& sessionToBindTo) : session (sessionToBindTo)
{
    for (int i = 0; i < Session::kNumTracks; ++i)
        strips[(size_t) i].bind (session.track (i).strip);
    for (int i = 0; i < Session::kNumAuxBuses; ++i)
        auxStrips[(size_t) i].bind (session.aux (i).strip);
    master.bind (session.master());

    deviceManager.initialiseWithDefaultDevices (16, 2);
    deviceManager.addAudioCallback (this);
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
        recordManager.stopRecording (transport.getPlayhead());

    playbackEngine.stopPlayback();
    transport.setPlayhead (0);
}

void AudioEngine::record()
{
    if (transport.isRecording()) return;
    if (! session.anyTrackArmed()) return;

    const double sr = currentSampleRate.load (std::memory_order_relaxed);
    if (sr <= 0.0) return;

    if (! recordManager.startRecording (sr, transport.getPlayhead()))
        return;

    playbackEngine.preparePlayback();  // un-armed tracks still play through
    transport.setState (Transport::State::Recording);
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const double sr = device->getCurrentSampleRate();
    const int    bs = device->getCurrentBufferSizeSamples();
    currentSampleRate.store (sr, std::memory_order_relaxed);
    currentBlockSize.store  (bs, std::memory_order_relaxed);

    for (auto& s : strips)    s.prepare (sr, bs);
    for (auto& a : auxStrips) a.prepare (sr, bs);
    master.prepare (sr, bs);
    playbackEngine.prepare (bs);  // size the playback read scratch — audio thread mustn't allocate

    mixL.assign ((size_t) bs, 0.0f);
    mixR.assign ((size_t) bs, 0.0f);
    for (auto& v : auxL) v.assign ((size_t) bs, 0.0f);
    for (auto& v : auxR) v.assign ((size_t) bs, 0.0f);
    playbackScratch.assign ((size_t) bs, 0.0f);
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
    const auto callbackStart = juce::Time::getHighResolutionTicks();

    if ((int) mixL.size() < numSamples)
    {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (auto* out = outputChannelData[ch])
                std::memset (out, 0, sizeof (float) * (size_t) numSamples);
        return;
    }

    std::memset (mixL.data(), 0, sizeof (float) * (size_t) numSamples);
    std::memset (mixR.data(), 0, sizeof (float) * (size_t) numSamples);
    for (auto& v : auxL) std::memset (v.data(), 0, sizeof (float) * (size_t) numSamples);
    for (auto& v : auxR) std::memset (v.data(), 0, sizeof (float) * (size_t) numSamples);

    const bool anyChannelSolo = session.anyTrackSoloed();
    const bool anyAuxSolo     = session.anyAuxSoloed();

    std::array<float*, ChannelStrip::kNumBuses> busLPtrs {};
    std::array<float*, ChannelStrip::kNumBuses> busRPtrs {};
    for (int a = 0; a < Session::kNumAuxBuses; ++a)
    {
        busLPtrs[(size_t) a] = auxL[(size_t) a].data();
        busRPtrs[(size_t) a] = auxR[(size_t) a].data();
    }

    const auto state = transport.getState();
    const bool isPlaying   = (state == Transport::State::Playing);
    const bool isRecording = (state == Transport::State::Recording);
    const juce::int64 blockStartSamples = transport.getPlayhead();

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& trackParams = session.track (t).strip;
        const bool muted   = trackParams.mute.load (std::memory_order_relaxed);
        const bool soloed  = trackParams.solo.load (std::memory_order_relaxed);
        const bool armed   = session.track (t).recordArmed.load (std::memory_order_relaxed);
        // Monitor-off on an armed track silences the master output (engineer
        // is monitoring direct from hardware). Un-armed playback tracks
        // ignore monitor state.
        const bool monitorPasses = armed
            ? session.track (t).inputMonitor.load (std::memory_order_relaxed)
            : true;
        const bool passes = ! muted && (anyChannelSolo ? soloed : true) && monitorPasses;

        // Resolve the input source for this track.
        const int inputIdx = session.resolveInputForTrack (t);
        const float* deviceInput = (inputIdx >= 0 && inputIdx < numInputChannels)
                                    ? inputChannelData[(size_t) inputIdx] : nullptr;
        const bool monitorEnabled = session.track (t).inputMonitor.load (std::memory_order_relaxed);

        // Choose the source the channel strip will process.
        //   - Un-armed track during playback: read from disk.
        //   - Un-armed track + IN on: live monitoring through the strip.
        //   - Armed track: live input ALWAYS feeds the strip — even with IN
        //     off — so a PRINT-mode recorder can grab the post-effects
        //     buffer. The IN toggle gates the master accumulation
        //     (passByGate / monitorPasses), not whether the strip processes.
        //   - Un-armed + IN off + stopped: monoIn null → strip is silent.
        const float* monoIn = nullptr;

        if ((isPlaying || isRecording) && ! armed)
        {
            playbackEngine.readForTrack (t, blockStartSamples,
                                          playbackScratch.data(), numSamples);
            monoIn = playbackScratch.data();
        }
        else if (deviceInput != nullptr && (armed || monitorEnabled))
        {
            monoIn = deviceInput;
        }

        // Input meter — armed tracks always show live input (so the user can
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

        float inputPeak = 0.0f;
        if (meterSrc != nullptr)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float a = std::abs (meterSrc[i]);
                if (a > inputPeak) inputPeak = a;
            }
        }
        const float inputDb = (inputPeak > 1e-5f)
                              ? juce::Decibels::gainToDecibels (inputPeak, -100.0f)
                              : -100.0f;
        session.track (t).meterInputDb.store (inputDb, std::memory_order_relaxed);

        // Tell the strip whether the recorder is going to ask for the
        // post-effects buffer this block — if so, the strip MUST run its
        // DSP even when it's not passing to master. Otherwise we let it
        // skip the heavy pass to save CPU on silent tracks.
        const bool needPrintBuffer = isRecording && armed && deviceInput != nullptr
                                  && session.track (t).printEffects.load (std::memory_order_relaxed);
        strips[(size_t) t].setNeedsProcessedMono (needPrintBuffer);

        strips[(size_t) t].processAndAccumulate (monoIn,
                                                 mixL.data(), mixR.data(),
                                                 busLPtrs, busRPtrs,
                                                 numSamples,
                                                 passes && monoIn != nullptr);
        session.track (t).meterGrDb.store (strips[(size_t) t].getCurrentGrDb(),
                                            std::memory_order_relaxed);

        // Recording capture — armed tracks always commit their input to
        // disk while recording. By default the raw deviceInput is written;
        // when `printEffects` is engaged, the post-EQ/post-comp buffer that
        // the strip just produced is written instead, "printing" the
        // channel-strip processing to the WAV.
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
            recordManager.writeInputBlock (t, recSrc, numSamples);
        }
    }

    for (int a = 0; a < Session::kNumAuxBuses; ++a)
    {
        const auto& params = session.aux (a).strip;
        const bool muted   = params.mute.load (std::memory_order_relaxed);
        const bool soloed  = params.solo.load (std::memory_order_relaxed);
        const bool passes  = ! muted && (anyAuxSolo ? soloed : true);

        if (! passes) continue;

        auxStrips[(size_t) a].processInPlace (auxL[(size_t) a].data(),
                                              auxR[(size_t) a].data(),
                                              numSamples);
        for (int i = 0; i < numSamples; ++i)
        {
            mixL[(size_t) i] += auxL[(size_t) a][(size_t) i];
            mixR[(size_t) i] += auxR[(size_t) a][(size_t) i];
        }
    }

    master.processInPlace (mixL.data(), mixR.data(), numSamples);

    if (numOutputChannels >= 1 && outputChannelData[0] != nullptr)
        std::memcpy (outputChannelData[0], mixL.data(), sizeof (float) * (size_t) numSamples);
    if (numOutputChannels >= 2 && outputChannelData[1] != nullptr)
        std::memcpy (outputChannelData[1], mixR.data(), sizeof (float) * (size_t) numSamples);
    for (int ch = 2; ch < numOutputChannels; ++ch)
        if (auto* out = outputChannelData[ch])
            std::memset (out, 0, sizeof (float) * (size_t) numSamples);

    if (isPlaying || isRecording)
        transport.advancePlayhead (numSamples);

    // Detect xrun: callback work shouldn't exceed the buffer's wall-clock
    // budget. If it does, we'd glitch on the next callback. Track the count
    // for the status bar.
    const auto sr = currentSampleRate.load (std::memory_order_relaxed);
    if (sr > 0.0)
    {
        const double bufferMs = 1000.0 * (double) numSamples / sr;
        const double elapsedMs =
            juce::Time::highResolutionTicksToSeconds (
                juce::Time::getHighResolutionTicks() - callbackStart) * 1000.0;
        if (elapsedMs > bufferMs)
            xrunCount.fetch_add (1, std::memory_order_relaxed);
    }
}
} // namespace adhdaw
