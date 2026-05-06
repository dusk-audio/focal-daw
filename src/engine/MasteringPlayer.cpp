#include "MasteringPlayer.h"
#include <cstring>

namespace adhdaw
{
MasteringPlayer::MasteringPlayer()
{
    formatManager.registerBasicFormats();
}

void MasteringPlayer::prepare (int maxBlockSize)
{
    readScratch.setSize (2, juce::jmax (1, maxBlockSize),
                          /*keepExistingContent*/ false,
                          /*clearExtraSpace*/      false,
                          /*avoidReallocating*/    false);
}

bool MasteringPlayer::loadFile (const juce::File& file)
{
    unloadFile();
    if (! file.existsAsFile()) return false;

    std::unique_ptr<juce::AudioFormatReader> r (formatManager.createReaderFor (file));
    if (r == nullptr) return false;

    reader = std::move (r);
    loadedFile = file;
    playhead.store (0, std::memory_order_relaxed);
    playing.store (false, std::memory_order_relaxed);
    return true;
}

void MasteringPlayer::unloadFile()
{
    playing.store (false, std::memory_order_relaxed);
    reader.reset();
    loadedFile = juce::File();
    playhead.store (0, std::memory_order_relaxed);
}

juce::int64 MasteringPlayer::getLengthSamples() const noexcept
{
    return reader ? reader->lengthInSamples : 0;
}

double MasteringPlayer::getSourceSampleRate() const noexcept
{
    return reader ? reader->sampleRate : 0.0;
}

void MasteringPlayer::process (float* L, float* R, int numSamples) noexcept
{
    if (L == nullptr || R == nullptr) return;
    std::memset (L, 0, sizeof (float) * (size_t) numSamples);
    std::memset (R, 0, sizeof (float) * (size_t) numSamples);

    if (reader == nullptr) return;
    if (! playing.load (std::memory_order_relaxed)) return;

    const juce::int64 start = playhead.load (std::memory_order_relaxed);
    if (start < 0) return;
    if (start >= reader->lengthInSamples)
    {
        // Past EOF - auto-stop so the UI can flip the Play button back.
        playing.store (false, std::memory_order_relaxed);
        return;
    }

    const int  available = (int) juce::jmin ((juce::int64) numSamples,
                                              reader->lengthInSamples - start);
    if (available > readScratch.getNumSamples()) return;  // shouldn't happen

    // Read into our 2-ch scratch. AudioFormatReader::read with two non-null
    // destination pointers fills both; for mono sources it duplicates the
    // single channel into both, which is exactly what we want.
    reader->read (&readScratch, 0, available, start,
                   /*useLeftChan*/  true,
                   /*useRightChan*/ true);

    std::memcpy (L, readScratch.getReadPointer (0), sizeof (float) * (size_t) available);
    std::memcpy (R, readScratch.getReadPointer (1), sizeof (float) * (size_t) available);

    playhead.fetch_add (available, std::memory_order_relaxed);

    // If we just hit EOF mid-block, auto-stop.
    if (start + available >= reader->lengthInSamples)
        playing.store (false, std::memory_order_relaxed);
}
} // namespace adhdaw
