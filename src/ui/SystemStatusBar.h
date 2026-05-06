#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/AudioEngine.h"

namespace focal
{
// Compact monospace readout that lives in the upper-right of the window:
//   Audio: 48 kHz 5.3 ms   DSP: 12% (3)
// Updated 4 Hz. CPU comes from AudioDeviceManager; xrun count from AudioEngine.
class SystemStatusBar final : public juce::Component, private juce::Timer
{
public:
    explicit SystemStatusBar (AudioEngine& engineRef);
    ~SystemStatusBar() override;

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    AudioEngine& engine;
    juce::String audioInfo  { "Audio: -" };
    juce::String dspInfo    { "DSP: -"   };
};
} // namespace focal
