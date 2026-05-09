#include "PlaybackEngine.h"
#include <cstring>

namespace focal
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
    // Pre-allocate the stereo scratch buffer once. Channel 1 is unused for
    // mono regions but allocated so the audio-thread read path never has to
    // grow on a stereo region.
    readScratch.setSize (2, juce::jmax (1, maxBlockSize),
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
            rs.fadeInSamples   = juce::jmax ((juce::int64) 0, region.fadeInSamples);
            rs.fadeOutSamples  = juce::jmax ((juce::int64) 0, region.fadeOutSamples);
            rs.numChannels     = juce::jlimit (1, 2, region.numChannels);
            // Convert dB once on the message thread; the audio loop
            // multiplies by the linear factor per sample. Clamp the
            // dB at extreme values to avoid wild values from a hand-
            // edited session.json producing audible clip on first
            // play. The Alt-drag clamps tighter ([-24, +12]) at the UI.
            rs.gainLinear = juce::Decibels::decibelsToGain (
                juce::jlimit (-60.0f, 24.0f, region.gainDb), -60.0f);
            // Enforce non-overlap: if fadeIn + fadeOut > length the multiplied
            // ramps produce a gain-notch in the middle. Shrink proportionally
            // so the ramps meet at a single sample instead.
            if (rs.fadeInSamples + rs.fadeOutSamples > rs.lengthInSamples)
            {
                const auto total = rs.fadeInSamples + rs.fadeOutSamples;
                if (total > 0)
                {
                    rs.fadeInSamples = (rs.fadeInSamples * rs.lengthInSamples) / total;
                    rs.fadeOutSamples = rs.lengthInSamples - rs.fadeInSamples;
                }
            }
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
                                   float* outL,
                                   float* outR,
                                   int numSamples) noexcept
{
    if (outL == nullptr) return;
    std::memset (outL, 0, sizeof (float) * (size_t) numSamples);
    if (outR != nullptr)
        std::memset (outR, 0, sizeof (float) * (size_t) numSamples);

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
        // For mono regions, read L only. For stereo, read both. The
        // BufferingAudioReader's read(useLeft, useRight) flags do the
        // right thing on either side; we always have a 2-channel
        // readScratch so the call is safe regardless.
        const bool readStereo = (r.numChannels == 2);
        r.reader->read (&readScratch, 0, withinSamples, readStart,
                         /*useLeftChan*/ true,
                         /*useRightChan*/ readStereo);

        // Apply fade-in / fade-out envelope in scratch, then SUM (instead
        // of REPLACE) into the output buffer(s). Summing lets two regions
        // overlap during a crossfade window. Mono regions duplicate the
        // L channel into outR (when outR is non-null) so the strip's
        // stereo path sees a center-panned signal.
        const juce::int64 fadeIn  = r.fadeInSamples;
        const juce::int64 fadeOut = r.fadeOutSamples;
        const juce::int64 regionStart = r.timelineStart;
        const juce::int64 fadeInGain = (fadeIn > 0) ? fadeIn : 1;
        const juce::int64 fadeOutGain = (fadeOut > 0) ? fadeOut : 1;
        const auto* srcL = readScratch.getReadPointer (0);
        const auto* srcR = readStereo ? readScratch.getReadPointer (1) : srcL;
        const float regionGain = r.gainLinear;
        for (int i = 0; i < withinSamples; ++i)
        {
            const juce::int64 timelineSample = firstWithin + i;
            float gain = regionGain;
            if (fadeIn > 0)
            {
                const juce::int64 inPos = timelineSample - regionStart;
                if (inPos < fadeIn)
                    gain *= juce::jlimit (0.0f, 1.0f,
                                            (float) inPos / (float) fadeInGain);
            }
            if (fadeOut > 0)
            {
                const juce::int64 outPos = regionEnd - timelineSample;
                if (outPos < fadeOut)
                    gain *= juce::jlimit (0.0f, 1.0f,
                                            (float) outPos / (float) fadeOutGain);
            }
            outL[outOffset + i] += srcL[i] * gain;
            if (outR != nullptr)
                outR[outOffset + i] += srcR[i] * gain;
        }
    }
}
} // namespace focal
