#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace focal
{
// Per-app plugin host infrastructure. Owns the AudioPluginFormatManager and
// the shared KnownPluginList (the "what's installed" cache). Channel strips
// hold their own PluginSlot which calls back here to instantiate plugins.
//
// MVP: registers JUCE's default formats (VST3 and any platform-natives that
// happen to compile in). NO automatic scan-on-startup - that's slow and
// unbounded; deferred to a later stage that runs scanning on a background
// thread with progress UI. For now, plugins are loaded by file path via
// `createPluginInstance(File)` and we add to KnownPluginList as we go so
// session save/restore can re-instantiate by description.
//
// Lifetime: a single PluginManager instance lives in AudioEngine for now
// (so the audio engine owns plugin infrastructure alongside the device
// manager). Could move to FocalApp scope later if non-engine code needs
// access.
class PluginManager
{
public:
    PluginManager();
    ~PluginManager();

    juce::AudioPluginFormatManager& getFormatManager() noexcept { return formatManager; }
    juce::KnownPluginList&          getKnownPluginList() noexcept { return knownPluginList; }

    // Instantiate a plugin from a file path (e.g. a .vst3 bundle directory
    // on Linux). Returns nullptr on failure (file missing, format not
    // supported, plugin failed to load). Synchronous - may take 100s of ms
    // for a slow plugin's instantiation. Caller is responsible for calling
    // prepareToPlay before processing audio through the returned instance.
    //
    // On success the discovered PluginDescription is added to
    // knownPluginList so future session loads can resolve by description.
    std::unique_ptr<juce::AudioPluginInstance>
    createPluginInstance (const juce::File& pluginFile,
                           double sampleRate, int blockSize,
                           juce::String& errorMessage);

    // Same shape but resolve by a previously-discovered PluginDescription
    // (used when restoring a session - the plugin file path may have moved
    // but the description's uid + format matches).
    std::unique_ptr<juce::AudioPluginInstance>
    createPluginInstance (const juce::PluginDescription& desc,
                           double sampleRate, int blockSize,
                           juce::String& errorMessage);

    // Cache file lives at ~/.config/Focal/plugin-cache.xml (Linux). Saved
    // every time a plugin is successfully added to knownPluginList; loaded
    // (best-effort) at construction. Invalid entries are tolerated - JUCE's
    // KnownPluginList::recreateFromXml silently skips bad ones.
    juce::File getCacheFile() const;

    // Scans every supported plugin format's default install locations and
    // populates knownPluginList. Synchronous; can take 10-30 seconds the
    // first time on a system with many plugins. Returns the number of
    // plugins added (existing entries are not re-added). UI should run
    // this from a "Scanning..." modal so the user knows the app isn't
    // hung. Cache is auto-saved on success.
    int scanInstalledPlugins();

private:
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList          knownPluginList;

    void loadCache();
    void saveCache() const;
};
} // namespace focal
