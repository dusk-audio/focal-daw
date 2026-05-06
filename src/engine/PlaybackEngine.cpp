#include "PlaybackEngine.h"
#include <cstring>

namespace adhdaw
{
PlaybackEngine::PlaybackEngine (Session& s) : session (s)
{
    formatManager.registerBasicFormats();

    // Start the prefetch thread once at construction. BufferingAudioReader
    // instances created later in preparePlayback() attach themselves via
    // addTimeSliceClient (handled by the BufferingAudioReader ctor) and
    // detach on destruction in stopPlayback().
    bufferingThread.startThread();
}

PlaybackEngine::~PlaybackEngine()
{
    // Tear down readers (which detach from bufferingThread) before letting
    // the thread member go out of scope. stopPlayback handles the readers;
    // ~TimeSliceThread joins.
    stopPlayback();
    bufferingThread.stopThread (2000);
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

        auto stream = std::make_unique<PerTrackStream>();
        stream->regions.reserve (regions.size());

        for (const auto& region : regions)
        {
            if (! region.file.existsAsFile()) continue;

            std::unique_ptr<juce::AudioFormatReader> rawReader (
                formatManager.createReaderFor (region.file));
            if (rawReader == nullptr) continue;

            // Wrap in a BufferingAudioReader. Sample rate of the source isn't
            // known at this scope without inspecting `rawReader`, so size by
            // a fixed sample count: 96000 samples is ~1 s at 96 kHz / ~2 s at
            // 44.1 kHz - generous given block sizes are 256–2048. Read
            // timeout 0 keeps the audio thread non-blocking; missed reads
            // return silence until prefetch catches up.
            constexpr int kSamplesToBuffer = 96000;
            auto buffered = std::make_unique<juce::BufferingAudioReader> (
                rawReader.release(), bufferingThread, kSamplesToBuffer);
            buffered->setReadTimeout (0);

            RegionStream rs;
            rs.reader          = std::move (buffered);
            rs.timelineStart   = region.timelineStart;
            rs.lengthInSamples = region.lengthInSamples;
            rs.sourceOffset    = region.sourceOffset;
            stream->regions.push_back (std::move (rs));
        }

        // Sort by timelineStart so the audio thread can stop iterating as
        // soon as it sees a region beyond the current block. Equal starts
        // preserve insertion order so the most-recently-recorded take wins
        // on overlap (recorder appends to the back of session.regions).
        std::stable_sort (stream->regions.begin(), stream->regions.end(),
                           [] (const RegionStream& a, const RegionStream& b)
                           {
                               return a.timelineStart < b.timelineStart;
                           });

        if (! stream->regions.empty())
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
    if (output == nullptr) return;
    std::memset (output, 0, sizeof (float) * (size_t) numSamples);

    auto& slot = streams[(size_t) trackIndex];
    if (slot == nullptr) return;

    const juce::int64 blockEnd = playheadSamples + numSamples;

    for (auto& r : slot->regions)
    {
        if (r.reader == nullptr) continue;

        // Regions are sorted by timelineStart - once we see one that begins
        // past the block, no later region can overlap us either.
        if (r.timelineStart >= blockEnd) break;

        const juce::int64 regionEnd = r.timelineStart + r.lengthInSamples;
        if (regionEnd <= playheadSamples) continue;  // already past

        const juce::int64 firstWithin = juce::jmax (playheadSamples, r.timelineStart);
        const juce::int64 lastWithin  = juce::jmin (blockEnd, regionEnd);
        const int outOffset    = (int) (firstWithin - playheadSamples);
        const int withinSamples = (int) (lastWithin - firstWithin);
        if (withinSamples <= 0) continue;
        // If this fires, prepare() was called with a maxBlockSize smaller than
        // the host's actual block size. Skip silently in release so we don't
        // crash, but make the misconfiguration visible in debug.
        jassert (withinSamples <= readScratch.getNumSamples());
        if (withinSamples > readScratch.getNumSamples()) continue;

        const juce::int64 readStart = r.sourceOffset + (firstWithin - r.timelineStart);
        r.reader->read (&readScratch, 0, withinSamples, readStart,
                         /*useLeftChan*/ true, /*useRightChan*/ false);
        std::memcpy (output + outOffset,
                      readScratch.getReadPointer (0),
                      sizeof (float) * (size_t) withinSamples);
    }
}
} // namespace adhdaw
