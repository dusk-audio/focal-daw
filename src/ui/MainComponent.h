#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include "ADHDawLookAndFeel.h"
#include "ConsoleView.h"
#include "../engine/AudioEngine.h"
#include "../session/Session.h"

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

    Session session;
    AudioEngine engine { session };

    ADHDawLookAndFeel lookAndFeel;

    juce::TextButton audioSettingsButton { "Audio settings..." };
    juce::TextButton saveButton { "Save" };
    juce::TextButton loadButton { "Load" };
    juce::Label statusLabel;
    std::unique_ptr<ConsoleView> consoleView;
    std::unique_ptr<class TransportBar>     transportBar;
    std::unique_ptr<class TapeStrip>        tapeStrip;
    std::unique_ptr<class SystemStatusBar>  systemStatusBar;
    bool tapeStripExpanded = false;  // collapsed by default; user expands when arranging
};
} // namespace adhdaw
