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

        // 24-bit mono WAV per the spec ("Bit depth: 32-bit float internal,
        // record to 24-bit WAV files"). Dithering will land later; for the
        // recorder MVP we let JUCE's writer handle the float→int conversion.
        std::unique_ptr<juce::AudioFormatWriter> writer (
            wav.createWriterFor (fileStream, sampleRate, 1, 24, {}, 0));
        if (writer == nullptr)
        {
            delete fileStream;
            continue;
        }

        auto perTrack = std::make_unique<PerTrackWriter>();
        perTrack->file = outFile;
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
                                     const float* inputData,
                                     int numSamples) noexcept
{
    if (! active.load (std::memory_order_acquire)) return;
    auto& slot = writers[(size_t) trackIndex];
    if (slot == nullptr || slot->writer == nullptr || inputData == nullptr) return;

    const float* channels[1] = { inputData };
    if (slot->writer->write (channels, numSamples))
        slot->framesWritten += numSamples;
}
} // namespace focal
