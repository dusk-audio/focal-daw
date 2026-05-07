#include "RecordManager.h"

namespace focal
{
RecordManager::RecordManager (Session& s) : session (s)
{
    diskThread.startThread();
}

RecordManager::~RecordManager()
{
    if (active.load (std::memory_order_relaxed))
        stopRecording (0);
    diskThread.stopThread (2000);
}

bool RecordManager::startRecording (double sampleRate, juce::int64 startSample)
{
    if (active.load (std::memory_order_relaxed))
        return true;
    if (! session.anyTrackArmed())
        return false;

    auto audioDir = session.getAudioDirectory();
    if (! audioDir.exists())
        audioDir.createDirectory();

    recordStartSample = startSample;
    recordSampleRate  = sampleRate;

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d-%H%M%S");

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        if (! session.track (t).recordArmed.load (std::memory_order_relaxed))
            continue;

        auto trackName = juce::String::formatted ("track%02d_%s.wav", t + 1,
                                                   stamp.toRawUTF8());
        juce::File outFile = audioDir.getChildFile (trackName);
        outFile.deleteFile();

        auto* fileStream = outFile.createOutputStream().release();
        if (fileStream == nullptr)
            continue;

        // 24-bit WAV per the spec. Channel count follows the track's mode:
        // 1 for Mono / Midi (MIDI tracks don't audio-record yet), 2 for
        // Stereo. The writer's channel count is captured here so writeInput-
        // Block builds a matching channel-pointer array on the audio thread.
        const int trackChannels =
            session.track (t).mode.load (std::memory_order_relaxed)
                == (int) Track::Mode::Stereo ? 2 : 1;

        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (fileStream, sampleRate, (unsigned int) trackChannels,
                                  24, {}, 0));
        if (writer == nullptr)
        {
            delete fileStream;
            continue;
        }

        auto perTrack = std::make_unique<PerTrackWriter>();
        perTrack->file = outFile;
        perTrack->numChannels = trackChannels;
        perTrack->writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
            writer.release(), diskThread, 32768);
        writers[(size_t) t] = std::move (perTrack);
    }

    active.store (true, std::memory_order_release);
    return true;
}

void RecordManager::stopRecording (juce::int64 /*endSample*/)
{
    if (! active.load (std::memory_order_relaxed))
        return;

    active.store (false, std::memory_order_release);

    // Tear down writers (this flushes the threaded queues and closes the
    // WAV files), then commit a Region for each.
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& slot = writers[(size_t) t];
        if (slot == nullptr) continue;

        const auto frames = slot->framesWritten;
        slot->writer.reset();  // closes the file

        if (frames > 0)
        {
            AudioRegion region;
            region.file = slot->file;
            region.timelineStart = recordStartSample;
            region.lengthInSamples = frames;
            region.sourceOffset = 0;
            region.numChannels = slot->numChannels;

            // Take-history capture: any existing region whose timeline range
            // is FULLY CONTAINED within the new take's range gets absorbed
            // into previousTakes. The user can then cycle through them via
            // the badge UI without losing access to earlier takes.
            //
            // Partial overlaps (e.g. punch-in over the middle of a longer
            // take) are intentionally NOT absorbed - the longer region stays
            // visible on either side of the new take, and the painter just
            // draws the new region on top inside the punch range. Phase 3
            // proper will handle splitting a partially-overlapping region
            // into outer fragments + a new take cycle slot.
            const juce::int64 newStart = region.timelineStart;
            const juce::int64 newEnd   = newStart + region.lengthInSamples;
            auto& regs = session.track (t).regions;

            // Crossfade length: 10 ms per side. Short enough that the user
            // doesn't perceive it as a fade, long enough to mask boundary
            // discontinuities between takes. Bound by half the new take's
            // length so a punch shorter than 20 ms still gets symmetric
            // ramps without the in/out fades overlapping each other.
            const juce::int64 fadeSamplesNominal = (juce::int64) (recordSampleRate * 0.010);
            const juce::int64 fadeSamples = juce::jmax<juce::int64> (
                0, juce::jmin (fadeSamplesNominal, region.lengthInSamples / 2));

            // Pass 1 — fully-contained takes get absorbed into the new
            // region's previousTakes (no audio overlap, just history).
            // Partial overlaps fall through to Pass 2 below.
            std::vector<AudioRegion> spawnedFragments;
            for (auto it = regs.begin(); it != regs.end(); )
            {
                const auto exStart = it->timelineStart;
                const auto exEnd   = it->timelineStart + it->lengthInSamples;
                const bool fullyContained = exStart >= newStart && exEnd <= newEnd;
                if (! fullyContained) { ++it; continue; }

                TakeRef ref;
                ref.file            = it->file;
                ref.sourceOffset    = it->sourceOffset;
                ref.lengthInSamples = it->lengthInSamples;
                region.previousTakes.push_back (std::move (ref));

                // Carry forward the displaced region's own history so we
                // don't drop deeper takes when overdubbing repeatedly. The
                // newly-displaced take goes first, then the older ones.
                for (auto& deeper : it->previousTakes)
                    region.previousTakes.push_back (std::move (deeper));

                it = regs.erase (it);
            }

            // Pass 2 — partial overlaps get split / trimmed so the new
            // take's edges crossfade against the existing region's audio
            // instead of clicking. Three cases:
            //   • Left overlap  (exStart < newStart, exEnd inside punch):
            //     trim ex to [exStart, newStart + fade], fadeOut at end.
            //   • Right overlap (exStart inside punch, exEnd > newEnd):
            //     trim ex to [newEnd - fade, exEnd] + advance sourceOffset.
            //   • Span (ex wraps both ends): produce two fragments — left
            //     half + right half — sharing the original source file.
            // Fades are matched on the new region by hasOverlapL / R below.
            bool hasOverlapL = false, hasOverlapR = false;
            for (auto it = regs.begin(); it != regs.end(); )
            {
                const auto exStart = it->timelineStart;
                const auto exEnd   = it->timelineStart + it->lengthInSamples;
                const bool overlaps = ! (exEnd <= newStart || exStart >= newEnd);
                if (! overlaps) { ++it; continue; }

                const bool spansLeft  = exStart < newStart;
                const bool spansRight = exEnd   > newEnd;

                if (spansLeft && spansRight)
                {
                    // Span: produce a left fragment + a right fragment from
                    // the same source. Mutate `it` into the left fragment
                    // and queue the right fragment for re-insertion.
                    AudioRegion right = *it;
                    right.timelineStart   = newEnd - fadeSamples;
                    right.sourceOffset    = it->sourceOffset
                                           + (right.timelineStart - it->timelineStart);
                    right.lengthInSamples = exEnd - right.timelineStart;
                    right.fadeInSamples   = fadeSamples;
                    // Right fragment ends at the original exEnd, so any fade-out
                    // the source region carried still applies. Clamp so the new
                    // shorter length still satisfies fadeIn + fadeOut <= length.
                    right.fadeOutSamples  = juce::jmax<juce::int64> (0,
                        juce::jmin (right.fadeOutSamples,
                                     right.lengthInSamples - right.fadeInSamples));
                    right.previousTakes.clear();  // history stays with the left half
                    spawnedFragments.push_back (std::move (right));

                    it->lengthInSamples = (newStart + fadeSamples) - exStart;
                    it->fadeOutSamples  = fadeSamples;
                    hasOverlapL = hasOverlapR = true;
                    ++it;
                }
                else if (spansLeft)
                {
                    // Left overlap only: trim end to newStart + fade.
                    it->lengthInSamples = (newStart + fadeSamples) - exStart;
                    it->fadeOutSamples  = fadeSamples;
                    hasOverlapL = true;
                    ++it;
                }
                else if (spansRight)
                {
                    // Right overlap only: shift start to newEnd - fade.
                    const juce::int64 newLeft = newEnd - fadeSamples;
                    it->sourceOffset    += (newLeft - exStart);
                    it->timelineStart    = newLeft;
                    it->lengthInSamples  = exEnd - newLeft;
                    it->fadeInSamples    = fadeSamples;
                    hasOverlapR = true;
                    ++it;
                }
                else
                {
                    // Should be unreachable — fully-contained was handled
                    // in Pass 1. Defensive ++ to avoid an infinite loop.
                    ++it;
                }
            }
            for (auto& frag : spawnedFragments)
                regs.push_back (std::move (frag));

            if (hasOverlapL) region.fadeInSamples  = fadeSamples;
            if (hasOverlapR) region.fadeOutSamples = fadeSamples;

            regs.push_back (std::move (region));
        }
        else
        {
            slot->file.deleteFile();
        }
        slot.reset();
    }
}

void RecordManager::writeInputBlock (int trackIndex,
                                     const float* L,
                                     const float* R,
                                     int numSamples) noexcept
{
    if (! active.load (std::memory_order_acquire)) return;
    if (numSamples == 0) return;
    auto& slot = writers[(size_t) trackIndex];
    if (slot == nullptr || slot->writer == nullptr || L == nullptr) return;

    // Build the channel-pointer array to match the writer's channel count.
    // ThreadedWriter::write reads exactly numChannels pointers from the
    // array, so each slot it touches must be non-null.
    //   • Mono writer (numChannels == 1): only L is read; R is ignored even
    //     if the caller supplied it (mono-armed track + stereo input is a
    //     caller bug, asserted below).
    //   • Stereo writer (numChannels == 2): if R is null we duplicate L so
    //     the second channel is never a missing pointer.
    jassert (L != nullptr);
    const float* channels[2] = { L, (R != nullptr) ? R : L };
    jassert (channels[0] != nullptr
             && (slot->numChannels < 2 || channels[1] != nullptr));
    if (slot->writer->write (channels, numSamples))
        slot->framesWritten += numSamples;
}
} // namespace focal
