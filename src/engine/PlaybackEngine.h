#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <array>
#include <memory>
#include "../session/Session.h"

namespace adhdaw
{
// Phase 2 minimum: one mono playback stream per track. For the MVP we read
// directly via juce::AudioFormatReader — the OS file cache absorbs the disk
// I/O after the first pass. A juce::BufferingAudioReader wrapper is the
// follow-up when we need true non-blocking streaming.
class PlaybackEngine
{
public:
    explicit PlaybackEngine (Session& s);

    // Message thread: pre-allocate the read scratch buffer for the largest
    // block we'll ever see. Must be called before the audio thread starts
    // calling readForTrack so the audio path never allocates.
    void prepare (int maxBlockSize);

    // Message thread: open readers for the (single) region on each track.
    void preparePlayback();
    // Message thread: close readers.
    void stopPlayback();

    // Audio thread: write `numSamples` of the active region's audio for
    // `trackIndex` at the given playhead. `output` is overwritten — silence
    // is written when there's no active region.
    void readForTrack (int trackIndex, juce::int64 playheadSamples,
                       float* output, int numSamples) noexcept;

private:
    Session& session;
    juce::AudioFormatManager formatManager;

    struct PerTrackStream
    {
        std::unique_ptr<juce::AudioFormatReader> reader;
        juce::int64 regionStart  = 0;
        juce::int64 regionLength = 0;
        juce::int64 sourceOffset = 0;
    };

    std::array<std::unique_ptr<PerTrackStream>, Session::kNumTracks> streams;

    // Pre-allocated mono scratch buffer for AudioFormatReader::read(). Sized
    // once in prepare(); reused on every audio-thread call to readForTrack.
    juce::AudioBuffer<float> readScratch;
};
} // namespace adhdaw
