#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <array>
#include <atomic>
#include <memory>
#include "../session/Session.h"

namespace focal
{
// Per-track threaded WAV writer. Lifetime: created on the message thread when
// recording starts, written to from the audio thread (lock-free queue), drained
// by a background TimeSliceThread, and finalized on the message thread when
// recording stops.
class RecordManager
{
public:
    explicit RecordManager (Session& s);
    ~RecordManager();

    // Called on the message thread.
    // Returns false if no tracks are armed.
    bool startRecording (double sampleRate, juce::int64 startSample);

    // Called on the message thread. Closes the writers, finalizes WAV files,
    // appends new AudioRegion entries to each recorded track.
    void stopRecording (juce::int64 endSample);

    // Audio-thread: write `numSamples` of input N for the corresponding armed
    // track. `L` is the left/mono channel; `R` is the right channel for
    // stereo tracks (nullptr for mono). The writer was created with the
    // matching channel count in startRecording; the implementation builds the
    // channel-pointer array to satisfy ThreadedWriter::write's contract that
    // exactly numChannels pointers are non-null. Passing R != nullptr on a
    // mono-armed track is a programming error (asserted; only L is written).
    // Empty blocks (numSamples == 0) early-return without touching the writer.
    void writeInputBlock (int trackIndex,
                            const float* L,
                            const float* R,
                            int numSamples) noexcept;

    bool isActive() const noexcept { return active.load (std::memory_order_relaxed); }

private:
    Session& session;

    struct PerTrackWriter
    {
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        juce::File file;
        juce::int64 framesWritten = 0;
        int numChannels = 1;  // matches the writer's channel count: 1 for mono
                              // tracks, 2 for stereo. Stamped onto the committed
                              // AudioRegion so PlaybackEngine reads back the
                              // right number of channels.
    };

    juce::TimeSliceThread diskThread { "Focal recorder" };
    juce::WavAudioFormat wav;

    std::array<std::unique_ptr<PerTrackWriter>, Session::kNumTracks> writers;
    std::atomic<bool> active { false };

    juce::int64 recordStartSample = 0;
    double      recordSampleRate  = 0.0;
};
} // namespace focal
