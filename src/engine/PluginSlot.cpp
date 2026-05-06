#include "PluginSlot.h"
#include "PluginManager.h"
#include <cstring>

namespace adhdaw
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
    emptyMidi.clear();

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

void PluginSlot::processMonoBlock (float* monoData, int numSamples) noexcept
{
    if (bypassed.load (std::memory_order_relaxed)
        || autoBypassed.load (std::memory_order_relaxed))
        return;

    auto* p = currentInstance.load (std::memory_order_acquire);
    if (p == nullptr) return;

    // Time-budget watchdog. If the plugin's processBlock takes more than
    // `kBudgetFraction` of the buffer's audio time, auto-bypass - better a
    // silent plugin than a frozen audio thread. Wall-clock cost of the
    // measurement itself is one juce::Time::getHighResolutionTicks() pair
    // (~50-100 ns) so we always time, regardless of whether a plugin is
    // misbehaving.
    constexpr double kBudgetFraction = 0.6;  // 60 % of buffer audio time
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
        emptyMidi.clear();
        p->processBlock (buf, emptyMidi);
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
        emptyMidi.clear();
        p->processBlock (stereoScratch, emptyMidi);

        const float* l = stereoScratch.getReadPointer (0);
        const float* r = stereoScratch.getReadPointer (1);
        for (int i = 0; i < numSamples; ++i)
            monoData[i] = (l[i] + r[i]) * 0.5f;
    }

    if (bufferMs > 0.0)
    {
        const double elapsedMs = juce::Time::highResolutionTicksToSeconds (
            juce::Time::getHighResolutionTicks() - t0) * 1000.0;
        if (elapsedMs > bufferMs * kBudgetFraction)
            autoBypassed.store (true, std::memory_order_relaxed);
    }
}
} // namespace adhdaw
