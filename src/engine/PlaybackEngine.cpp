#include "PlaybackEngine.h"
#include <cstring>

namespace adhdaw
{
PlaybackEngine::PlaybackEngine (Session& s) : session (s)
{
    formatManager.registerBasicFormats();
}

void PlaybackEngine::prepare (int maxBlockSize)
{
    // Pre-allocate the mono scratch buffer once. AudioBuffer::setSize with
    // avoidReallocating=true keeps the existing allocation as long as we
    // don't grow past it; calling here with the largest expected block size
    // guarantees readForTrack on the audio thread never allocates.
    readScratch.setSize (1, juce::jmax (1, maxBlockSize),
                          /*keepExistingContent*/ false,
                          /*clearExtraSpace*/      false,
                          /*avoidReallocating*/    false);
}

void PlaybackEngine::preparePlayback()
{
    stopPlayback();

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& regions = session.track (t).regions;
        if (regions.empty()) continue;

        const auto& region = regions.front();
        if (! region.file.existsAsFile()) continue;

        std::unique_ptr<juce::AudioFormatReader> reader (
            formatManager.createReaderFor (region.file));
        if (reader == nullptr) continue;

        auto stream = std::make_unique<PerTrackStream>();
        stream->reader = std::move (reader);
        stream->regionStart  = region.timelineStart;
        stream->regionLength = region.lengthInSamples;
        stream->sourceOffset = region.sourceOffset;
        streams[(size_t) t] = std::move (stream);
    }
}

void PlaybackEngine::stopPlayback()
{
    for (auto& s : streams) s.reset();
}

void PlaybackEngine::readForTrack (int trackIndex,
                                   juce::int64 playheadSamples,
                                   float* output,
                                   int numSamples) noexcept
{
    auto& slot = streams[(size_t) trackIndex];
    if (output == nullptr) return;
    std::memset (output, 0, sizeof (float) * (size_t) numSamples);
    if (slot == nullptr || slot->reader == nullptr) return;

    const juce::int64 regionEnd = slot->regionStart + slot->regionLength;
    if (playheadSamples + numSamples <= slot->regionStart || playheadSamples >= regionEnd)
        return;

    const juce::int64 firstWithin = juce::jmax (playheadSamples, slot->regionStart);
    const juce::int64 lastWithin  = juce::jmin (playheadSamples + numSamples, regionEnd);
    const int leadSilence   = (int) (firstWithin - playheadSamples);
    const int withinSamples = (int) (lastWithin - firstWithin);
    if (withinSamples <= 0) return;

    const juce::int64 readStart = slot->sourceOffset + (firstWithin - slot->regionStart);

    // Reuse the pre-allocated scratch — no audio-thread allocation. If the
    // requested span ever exceeds what prepare() allocated we silently fall
    // back to a partial read (treat the overflow as silence) rather than
    // grow the buffer here.
    if (withinSamples > readScratch.getNumSamples())
        return;
    slot->reader->read (&readScratch, 0, withinSamples, readStart,
                        /*useLeftChan*/ true, /*useRightChan*/ false);
    std::memcpy (output + leadSilence,
                 readScratch.getReadPointer (0),
                 sizeof (float) * (size_t) withinSamples);
}
} // namespace adhdaw
