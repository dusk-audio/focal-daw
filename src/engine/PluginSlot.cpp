#include "PluginSlot.h"
#include "PluginManager.h"
#include <cstring>

namespace focal
{
PluginSlot::~PluginSlot()
{
    // Audio thread should already be detached by the time this runs (the
    // owning ChannelStrip destructs after its AudioEngine has released the
    // device callback). Belt-and-suspenders: clear the atomic first so
    // nothing reads from the instance during destruction.
    currentInstance.store (nullptr, std::memory_order_release);
    if (ownedInstance != nullptr)
        ownedInstance->releaseResources();
}

void PluginSlot::prepareToPlay (double sampleRate, int blockSize)
{
    preparedSampleRate = sampleRate;
    preparedBlockSize  = juce::jmax (1, blockSize);

    // Pre-size the stereo scratch so the audio thread never allocates when
    // a stereo-only plugin is in the slot.
    stereoScratch.setSize (2, preparedBlockSize, false, false, true);

    blocksSinceLoad     = 0;
    consecutiveOverruns = 0;

    if (ownedInstance != nullptr)
    {
        ownedInstance->setPlayConfigDetails (
            ownedInstance->getTotalNumInputChannels(),
            ownedInstance->getTotalNumOutputChannels(),
            sampleRate, preparedBlockSize);
        ownedInstance->prepareToPlay (sampleRate, preparedBlockSize);
    }
}

void PluginSlot::releaseResources()
{
    currentInstance.store (nullptr, std::memory_order_release);
    if (ownedInstance != nullptr)
        ownedInstance->releaseResources();
}

bool PluginSlot::isLoaded() const noexcept
{
    return currentInstance.load (std::memory_order_acquire) != nullptr;
}

juce::String PluginSlot::getLoadedName() const
{
    if (auto* p = currentInstance.load (std::memory_order_acquire))
        return p->getName();
    return {};
}

bool PluginSlot::loadFromFile (const juce::File& pluginFile, juce::String& errorMessage)
{
    if (manager == nullptr)
    {
        errorMessage = "PluginSlot has no PluginManager bound - call setManager() first";
        return false;
    }

    // Park the audio thread first. Detach the current instance, release it
    // off-thread (this destructor can be slow), then spin up the new one.
    currentInstance.store (nullptr, std::memory_order_release);

    auto previous = std::move (ownedInstance);  // released after the new load completes
    ownedInstance.reset();

    auto fresh = manager->createPluginInstance (pluginFile,
                                                  preparedSampleRate,
                                                  preparedBlockSize,
                                                  errorMessage);
    if (fresh == nullptr)
    {
        if (previous != nullptr) previous->releaseResources();  // explicit
        return false;
    }

    fresh->setPlayConfigDetails (fresh->getTotalNumInputChannels(),
                                  fresh->getTotalNumOutputChannels(),
                                  preparedSampleRate,
                                  preparedBlockSize);
    if (preparedSampleRate > 0.0)
        fresh->prepareToPlay (preparedSampleRate, preparedBlockSize);

    ownedInstance = std::move (fresh);
    blocksSinceLoad     = 0;
    consecutiveOverruns = 0;
    autoBypassed.store (false, std::memory_order_relaxed);
    currentInstance.store (ownedInstance.get(), std::memory_order_release);

    if (previous != nullptr) previous->releaseResources();
    return true;
}

bool PluginSlot::loadFromDescription (const juce::PluginDescription& desc,
                                        juce::String& errorMessage)
{
    if (manager == nullptr)
    {
        errorMessage = "PluginSlot has no PluginManager bound - call setManager() first";
        return false;
    }

    // Same swap-load shape as loadFromFile; just resolves via the
    // description path inside PluginManager.
    currentInstance.store (nullptr, std::memory_order_release);
    auto previous = std::move (ownedInstance);
    ownedInstance.reset();

    auto fresh = manager->createPluginInstance (desc, preparedSampleRate,
                                                  preparedBlockSize, errorMessage);
    if (fresh == nullptr)
    {
        if (previous != nullptr) previous->releaseResources();
        return false;
    }

    fresh->setPlayConfigDetails (fresh->getTotalNumInputChannels(),
                                  fresh->getTotalNumOutputChannels(),
                                  preparedSampleRate,
                                  preparedBlockSize);
    if (preparedSampleRate > 0.0)
        fresh->prepareToPlay (preparedSampleRate, preparedBlockSize);

    ownedInstance = std::move (fresh);
    blocksSinceLoad     = 0;
    consecutiveOverruns = 0;
    autoBypassed.store (false, std::memory_order_relaxed);
    currentInstance.store (ownedInstance.get(), std::memory_order_release);

    if (previous != nullptr) previous->releaseResources();
    return true;
}

void PluginSlot::unload()
{
    currentInstance.store (nullptr, std::memory_order_release);
    if (ownedInstance != nullptr)
    {
        ownedInstance->releaseResources();
        ownedInstance.reset();
    }
}

juce::String PluginSlot::getDescriptionXmlForSave() const
{
    auto* p = currentInstance.load (std::memory_order_acquire);
    if (p == nullptr) return {};
    juce::PluginDescription desc;
    p->fillInPluginDescription (desc);
    if (auto xml = desc.createXml())
        return xml->toString (juce::XmlElement::TextFormat().singleLine());
    return {};
}

juce::String PluginSlot::getStateBase64ForSave() const
{
    auto* p = currentInstance.load (std::memory_order_acquire);
    if (p == nullptr) return {};
    juce::MemoryBlock mb;
    p->getStateInformation (mb);
    return mb.toBase64Encoding();
}

bool PluginSlot::restoreFromSavedState (const juce::String& descriptionXml,
                                          const juce::String& stateBase64,
                                          juce::String& errorMessage)
{
    if (descriptionXml.isEmpty())
    {
        // Saved session had no plugin on this slot - make sure the slot is
        // empty. Returning success because "no plugin to restore" is the
        // valid steady state, not an error.
        unload();
        return true;
    }

    if (manager == nullptr)
    {
        errorMessage = "PluginSlot has no PluginManager bound";
        return false;
    }

    // Parse the description.
    auto xml = juce::XmlDocument::parse (descriptionXml);
    if (xml == nullptr)
    {
        errorMessage = "Saved plugin description is not valid XML";
        return false;
    }
    juce::PluginDescription desc;
    if (! desc.loadFromXml (*xml))
    {
        errorMessage = "Saved plugin description failed to deserialise";
        return false;
    }

    // Same swap-load shape as loadFromFile: park the audio thread, release
    // any previously-loaded plugin, instantiate, restore state, install.
    currentInstance.store (nullptr, std::memory_order_release);
    auto previous = std::move (ownedInstance);
    ownedInstance.reset();

    auto fresh = manager->createPluginInstance (desc, preparedSampleRate,
                                                  preparedBlockSize, errorMessage);
    if (fresh == nullptr)
    {
        if (previous != nullptr) previous->releaseResources();
        return false;
    }

    fresh->setPlayConfigDetails (fresh->getTotalNumInputChannels(),
                                  fresh->getTotalNumOutputChannels(),
                                  preparedSampleRate, preparedBlockSize);
    if (preparedSampleRate > 0.0)
        fresh->prepareToPlay (preparedSampleRate, preparedBlockSize);

    // Apply the saved state blob (if any).
    if (stateBase64.isNotEmpty())
    {
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding (stateBase64) && mb.getSize() > 0)
            fresh->setStateInformation (mb.getData(), (int) mb.getSize());
    }

    ownedInstance = std::move (fresh);
    currentInstance.store (ownedInstance.get(), std::memory_order_release);
    if (previous != nullptr) previous->releaseResources();
    return true;
}

void PluginSlot::processMonoBlock (float* monoData, int numSamples,
                                   juce::MidiBuffer& midiMessages) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    if (numSamples == 0) return;

    if (bypassed.load (std::memory_order_relaxed)
        || autoBypassed.load (std::memory_order_relaxed))
        return;

    auto* p = currentInstance.load (std::memory_order_acquire);
    if (p == nullptr) return;

    // Time-budget watchdog. A plugin that consistently overruns the buffer
    // gets auto-bypassed so it can't freeze the audio thread. Two
    // refinements over a naive single-block trip:
    //   • Warm-up grace: kGraceBlocks after a load (or prepareToPlay) the
    //     watchdog is silent. Reverbs / look-ahead limiters / oversamplers
    //     all do real work on their first few blocks (cold caches, internal
    //     ramps) and would otherwise be auto-bypassed before they ever
    //     produce wet output.
    //   • Consecutive-overrun threshold: a single late block (other-thread
    //     preemption, GC, kernel scheduling jitter) shouldn't kill a
    //     plugin. Require kMaxConsecutiveOverruns in a row.
    constexpr double kBudgetFraction         = 0.6;
    constexpr int    kGraceBlocks            = 16;
    constexpr int    kMaxConsecutiveOverruns = 4;
    const double bufferMs = (preparedSampleRate > 0.0)
                              ? 1000.0 * (double) numSamples / preparedSampleRate
                              : 0.0;
    const auto t0 = juce::Time::getHighResolutionTicks();

    const int numIn  = p->getTotalNumInputChannels();
    const int numOut = p->getTotalNumOutputChannels();

    if (numIn == 1 && numOut == 1)
    {
        // Mono in / mono out - process directly in place via a thin
        // AudioBuffer wrapper around the existing buffer.
        float* channels[1] = { monoData };
        juce::AudioBuffer<float> buf (channels, 1, numSamples);
        p->processBlock (buf, midiMessages);
    }
    else
    {
        // Stereo (or wider) plugin: duplicate mono → L+R, process, average
        // back to mono. Use the pre-allocated scratch so we don't touch
        // the heap.
        if (numSamples > stereoScratch.getNumSamples())
            return;

        stereoScratch.copyFrom (0, 0, monoData, numSamples);
        stereoScratch.copyFrom (1, 0, monoData, numSamples);
        p->processBlock (stereoScratch, midiMessages);

        const float* l = stereoScratch.getReadPointer (0);
        const float* r = stereoScratch.getReadPointer (1);
        for (int i = 0; i < numSamples; ++i)
            monoData[i] = (l[i] + r[i]) * 0.5f;
    }

    if (bufferMs > 0.0 && blocksSinceLoad >= kGraceBlocks)
    {
        const double elapsedMs = juce::Time::highResolutionTicksToSeconds (
            juce::Time::getHighResolutionTicks() - t0) * 1000.0;
        if (elapsedMs > bufferMs * kBudgetFraction)
        {
            if (++consecutiveOverruns >= kMaxConsecutiveOverruns)
                autoBypassed.store (true, std::memory_order_relaxed);
        }
        else
        {
            consecutiveOverruns = 0;
        }
    }
    if (blocksSinceLoad < kGraceBlocks) ++blocksSinceLoad;
}

void PluginSlot::processStereoBlock (float* L, float* R, int numSamples,
                                     juce::MidiBuffer& midiMessages) noexcept
{
    juce::ScopedNoDenormals noDenormals;
    if (numSamples == 0) return;

    if (bypassed.load (std::memory_order_relaxed)
        || autoBypassed.load (std::memory_order_relaxed))
        return;

    auto* p = currentInstance.load (std::memory_order_acquire);
    if (p == nullptr) return;

    constexpr double kBudgetFraction         = 0.6;
    constexpr int    kGraceBlocks            = 16;
    constexpr int    kMaxConsecutiveOverruns = 4;
    const double bufferMs = (preparedSampleRate > 0.0)
                              ? 1000.0 * (double) numSamples / preparedSampleRate
                              : 0.0;
    const auto t0 = juce::Time::getHighResolutionTicks();

    const int numIn  = p->getTotalNumInputChannels();
    const int numOut = p->getTotalNumOutputChannels();

    if (numIn >= 2 && numOut >= 2)
    {
        // Stereo plugin - wrap L/R as a 2-channel AudioBuffer and process in
        // place. Same shape as the per-aux EQ/comp pass above.
        float* channels[2] = { L, R };
        juce::AudioBuffer<float> buf (channels, 2, numSamples);
        p->processBlock (buf, midiMessages);
    }
    else if (numIn == 1 && numOut >= 1)
    {
        // Mono-input plugin on a stereo bus: collapse to mono, run, then fan
        // out the (possibly stereo) output back across L/R. Use the
        // pre-allocated stereoScratch as the working buffer.
        if (numSamples > stereoScratch.getNumSamples()) return;

        float* mono = stereoScratch.getWritePointer (0);
        for (int i = 0; i < numSamples; ++i)
            mono[i] = (L[i] + R[i]) * 0.5f;

        // Buffer width must be max(numIn, numOut) so the plugin can write
        // its stereo output. Pre-fill the extra channel with the mono mix
        // — JUCE's contract is that the plugin only reads numIn channels,
        // but copying mono there is harmless and avoids leaving uninit
        // memory in the buffer (which we otherwise read back as outR).
        const int procCh = juce::jmin (juce::jmax (numIn, numOut),
                                         stereoScratch.getNumChannels());
        for (int c = 1; c < procCh; ++c)
            stereoScratch.copyFrom (c, 0, mono, numSamples);

        float* procPtrs[2] = { stereoScratch.getWritePointer (0),
                               procCh > 1 ? stereoScratch.getWritePointer (1) : nullptr };
        juce::AudioBuffer<float> buf (procPtrs, procCh, numSamples);
        p->processBlock (buf, midiMessages);

        const float* outL = stereoScratch.getReadPointer (0);
        const float* outR = (numOut >= 2 && procCh >= 2)
                              ? stereoScratch.getReadPointer (1) : outL;
        std::memcpy (L, outL, sizeof (float) * (size_t) numSamples);
        std::memcpy (R, outR, sizeof (float) * (size_t) numSamples);
    }
    else
    {
        // Plugin layout we can't handle (zero outputs, etc.) - bail.
        return;
    }

    if (bufferMs > 0.0 && blocksSinceLoad >= kGraceBlocks)
    {
        const double elapsedMs = juce::Time::highResolutionTicksToSeconds (
            juce::Time::getHighResolutionTicks() - t0) * 1000.0;
        if (elapsedMs > bufferMs * kBudgetFraction)
        {
            if (++consecutiveOverruns >= kMaxConsecutiveOverruns)
                autoBypassed.store (true, std::memory_order_relaxed);
        }
        else
        {
            consecutiveOverruns = 0;
        }
    }
    if (blocksSinceLoad < kGraceBlocks) ++blocksSinceLoad;
}
} // namespace focal
