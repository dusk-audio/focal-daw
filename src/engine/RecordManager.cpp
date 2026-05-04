#include "RecordManager.h"

namespace adhdaw
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
            session.track (t).regions.push_back (std::move (region));
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
} // namespace adhdaw
