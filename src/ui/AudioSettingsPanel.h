#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

// ADHDaw-side declarations of the runtime ALSA periods accessors that live
// in our patched JUCE/modules/juce_audio_devices/native/juce_ALSA_linux.cpp.
// The default value is 4 to match upstream JUCE; UI writes here before
// triggering an AudioDeviceManager re-open so the new period count takes
// effect on the next snd_pcm_hw_params_set_periods_near call.
namespace juce
{
    JUCE_API void setALSARequestedPeriods (int p) noexcept;
    JUCE_API int  getALSARequestedPeriods() noexcept;
}

namespace adhdaw
{
class AudioEngine;
class Session;

// Wraps juce::AudioDeviceSelectorComponent and adds a Periods combo at the
// bottom. Periods is the only audio-config knob JUCE's stock selector does
// not expose, but it materially affects USB audio quality on Linux -
// fractional periods cause jitter, and certain plug+kernel combos produce
// distortion or xruns at specific counts. Exposing the knob lets the user
// tune for their hardware without rebuilding.
//
// Also hosts a "Run Self-Test" button that opens the SelfTestPanel - a
// headless test of the audio engine pipeline plus a backend cycle.
class AudioSettingsPanel final : public juce::Component
{
public:
    AudioSettingsPanel (juce::AudioDeviceManager& dm,
                         AudioEngine& engine,
                         Session& session);
    ~AudioSettingsPanel() override = default;

    void resized() override;

private:
    juce::AudioDeviceManager& deviceManager;
    AudioEngine& engine;
    Session& session;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> selector;

    juce::Label    periodsLabel  { {}, "Periods (ALSA)" };
    juce::ComboBox periodsCombo;
    juce::TextButton selfTestButton { "Run Self-Test..." };

    // Global effect oversampling - single source of truth for "1× / 2× / 4×
    // across all effects". Default 1× (lowest CPU). 2× / 4× engage internal
    // oversampling on master + aux bus comps and the master tape sat
    // oversampler. Per-channel comp + EQ stay at native rate regardless.
    juce::Label    oversamplingLabel { {}, "Effect Oversampling" };
    juce::ComboBox oversamplingCombo;

    juce::Label  uiScaleLabel  { {}, "UI scale" };
    juce::Slider uiScaleSlider;
    juce::Label  uiScaleHint;
    bool         uiScaleDragging = false;

    void applyPeriodsChange();
    void applyOversamplingChange();
    void applyUiScaleChange();
    void openSelfTest();
};
} // namespace adhdaw
