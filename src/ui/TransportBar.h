#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/AudioEngine.h"

namespace adhdaw
{
class TransportBar final : public juce::Component, private juce::Timer
{
public:
    explicit TransportBar (AudioEngine& engineRef);
    ~TransportBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void refreshButtonStates();

    AudioEngine& engine;
    juce::TextButton playButton    { "PLAY" };
    juce::TextButton stopButton    { "STOP" };
    juce::TextButton recordButton  { "REC"  };
    juce::TextButton tapeToggle    { juce::CharPointer_UTF8 ("\xe2\x96\xbe SUMMARY") };  // "▾ SUMMARY" — toggles the arrangement/summary view
    juce::Label      clockLabel;
    juce::Label      hintLabel;

public:
    // Set by MainComponent to receive toggle clicks. Fires after the button's
    // toggle state has flipped — the new collapsed/expanded state is the
    // boolean argument (true = expanded).
    std::function<void (bool)> onTapeStripToggle;

    void setTapeStripExpanded (bool expanded);
    bool isTapeStripExpanded() const;
};
} // namespace adhdaw
