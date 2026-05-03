#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/AudioEngine.h"

namespace adhdaw
{
class MainComponent final : public juce::Component
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void openAudioSettings();

    AudioEngine engine;
    juce::TextButton audioSettingsButton { "Audio settings..." };
    juce::Label statusLabel;
};
} // namespace adhdaw
