#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>

namespace focal
{
class PluginManager;

// A single plugin slot - holds at most one AudioPluginInstance and exposes
// audio-thread-safe processing methods. Designed for per-channel use:
// channel strips own a PluginSlot and call its `processMonoBlock` between
// phase-invert and EQ in their per-block processing.
//
// Threading rules (RT-critical):
//   • load() / unload() / setEnabled() run on the message thread only.
//   • processMonoBlock() / processStereoBlock() run on the audio thread.
//   • The instance pointer is wrapped in a std::atomic so swap-in / swap-out
//     are lock-free for the audio thread. Old instances are released on the
//     message thread after the swap (the instance destructor is NOT
//     RT-safe). The audio thread sees either nullptr (no plugin → bypass)
//     or a fully-prepared instance.
//
// MVP scope:
//   • No multi-bus support - mono in / mono out, with a stereo fallback when
//     a plugin can't do mono.
//   • No editor window management - that's a follow-up stage.
//   • No state save/restore via SessionSerializer yet - also follow-up.
class PluginSlot
{
public:
    PluginSlot() = default;
    ~PluginSlot();

    // Bind the manager that resolves plugin files. Must be called before
    // loadFromFile. Pointer rather than reference so the slot can be
    // default-constructed inside containers (e.g. as a ChannelStrip member,
    // which lives in std::array<ChannelStrip, 16> requiring default ctors).
    void setManager (PluginManager& mgr) noexcept { manager = &mgr; }

    // UI-side access to the bound manager - used by the ChannelStripComponent
    // plugin picker to walk KnownPluginList and trigger scans. Returns the
    // bound manager; binding must have happened first (asserts otherwise).
    PluginManager& getManagerForUi() const noexcept { jassert (manager != nullptr); return *manager; }

    // Message-thread API.
    void prepareToPlay (double sampleRate, int blockSize);
    void releaseResources();

    // Loads the plugin from a file. Returns true on success, false with
    // errorMessage populated on failure. Releases any previously-loaded
    // plugin first.
    bool loadFromFile (const juce::File& pluginFile, juce::String& errorMessage);

    // Same shape, but resolves via a previously-discovered PluginDescription
    // (e.g. one the user picked from the scanned KnownPluginList). Useful
    // when we want format-aware loading without having the user navigate to
    // the on-disk file path.
    bool loadFromDescription (const juce::PluginDescription& desc,
                                juce::String& errorMessage);

    // Unload - clears the slot. Audio thread sees nullptr next block.
    void unload();

    // True when an instance is currently loaded (and presumed processing).
    bool isLoaded() const noexcept;

    // Display name for the loaded plugin, or empty string if none.
    juce::String getLoadedName() const;

    // Bypass toggle - when true, processMonoBlock is a no-op even with a
    // plugin loaded. Useful for the time-budget-bypass mechanism described
    // in the spec for crash safety / CPU spikes.
    void setBypassed (bool shouldBypass) noexcept { bypassed.store (shouldBypass, std::memory_order_relaxed); }
    bool isBypassed() const noexcept             { return bypassed.load (std::memory_order_relaxed); }

    // Audio-thread API. Mono channel strip: feed the same buffer in and out;
    // the plugin processes it in place. If no plugin loaded or bypassed, the
    // buffer is unchanged.
    //
    // The buffer is treated as 1 channel of `numSamples`. For plugins that
    // require stereo (no mono bus support), we duplicate-and-process (L=R)
    // and average back to mono.
    //
    // `midiMessages` is forwarded to the plugin's processBlock. For audio-
    // effect inserts the caller passes an empty buffer; for instrument
    // plugins it carries the per-track-filtered MIDI events for the block.
    //
    // Stage-4 time-budget protection: each block is timed; if the plugin's
    // wall-clock exceeds `kTimeBudgetFraction` of the buffer's audio time
    // (e.g. 60 % of 21.3 ms at 1024/48k = 12.8 ms), the bypass flag is
    // automatically engaged. Engineering-grade safety: stops a misbehaving
    // plugin from freezing the audio thread even if it doesn't outright
    // crash. User can manually clear the auto-bypass via the plugin slot
    // UI (right-click → "Re-enable plugin"); we don't auto-recover because
    // the plugin would just freeze the thread again.
    void processMonoBlock (float* monoData, int numSamples,
                           juce::MidiBuffer& midiMessages) noexcept;

    // Stereo variant for aux/master buses. L and R are processed in place.
    // Mono-only plugins: average L+R into a temporary mono buffer, run the
    // plugin, write the result to both channels (so a mono reverb still
    // makes sense on a stereo send return).
    // Same time-budget watchdog as processMonoBlock.
    void processStereoBlock (float* L, float* R, int numSamples,
                             juce::MidiBuffer& midiMessages) noexcept;

    // True if the slot was AUTO-bypassed by the time-budget watchdog (as
    // distinct from manual setBypassed). UI shows this state so the user
    // knows why their plugin stopped affecting audio.
    bool wasAutoBypassed() const noexcept { return autoBypassed.load (std::memory_order_relaxed); }
    void clearAutoBypass() noexcept { autoBypassed.store (false, std::memory_order_relaxed); }

    // Plugin instance access for Stage 3 editor window. Returns nullptr if
    // no plugin loaded. Caller MUST be on the message thread; the audio
    // thread may swap the instance out between calls.
    juce::AudioPluginInstance* getInstance() const noexcept
    {
        return currentInstance.load (std::memory_order_acquire);
    }

    // Reported latency of the loaded plugin in samples. 0 when no plugin
    // is loaded or the plugin reports no latency. Audio-thread-safe -
    // reads the instance via the same atomic pointer the process path
    // uses, then queries getLatencySamples on the instance (no allocation,
    // no lock). Used by AudioEngine's MIDI scheduler to push instrument
    // events forward in time so the plugin's delayed audio output lands
    // on the correct timeline sample.
    int getLatencySamples() const noexcept
    {
        if (auto* p = currentInstance.load (std::memory_order_acquire))
            return p->getLatencySamples();
        return 0;
    }

    // Session save/restore. The XML form encodes a juce::PluginDescription
    // (uid + format + path + display name); the state blob is whatever
    // opaque bytes the plugin wants persisted (its getStateInformation
    // output). Both are empty strings when no plugin is loaded.
    juce::String getDescriptionXmlForSave() const;
    juce::String getStateBase64ForSave() const;

    // Re-create the plugin from the saved description and apply the state
    // blob. Returns true on success. Caller is responsible for ensuring
    // prepareToPlay has been called first; if the saved description doesn't
    // resolve to an installed plugin the slot is left empty.
    bool restoreFromSavedState (const juce::String& descriptionXml,
                                  const juce::String& stateBase64,
                                  juce::String& errorMessage);

private:
    PluginManager* manager = nullptr;

    // Owning pointer - only the message thread mutates this. The audio
    // thread reads via the `currentInstance` atomic below.
    std::unique_ptr<juce::AudioPluginInstance> ownedInstance;

    // Atomic view of `ownedInstance.get()` so the audio thread can
    // swap-load without a lock. Set after prepareToPlay completes.
    std::atomic<juce::AudioPluginInstance*> currentInstance { nullptr };
    std::atomic<bool> bypassed { false };
    std::atomic<bool> autoBypassed { false };  // tripped by the time-budget watchdog

    double preparedSampleRate = 0.0;
    int    preparedBlockSize  = 0;

    // Watchdog state. Audio-thread-only; no other thread reads these.
    //   • blocksSinceLoad: skips the budget check while a freshly-loaded
    //     plugin warms up (cold caches, lazy-init, oversampler ramps).
    //     Without this, plugins like reverbs and look-ahead limiters trip
    //     the watchdog on block 1 every time and never get processed.
    //   • consecutiveOverruns: a single late block (scheduling jitter,
    //     other-thread preemption) shouldn't kill the plugin. Require N
    //     in a row before bypassing.
    // Reset to 0 by prepareToPlay and by every successful load.
    int blocksSinceLoad     = 0;
    int consecutiveOverruns = 0;

    // Per-block scratch buffer for stereo-fallback plugins. Sized at
    // prepareToPlay so the audio thread doesn't allocate. 2-channel.
    juce::AudioBuffer<float> stereoScratch;
};
} // namespace focal
