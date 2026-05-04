#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../session/Session.h"

namespace adhdaw
{
class ChannelCompEditor final : public juce::Component, private juce::Timer
{
public:
    explicit ChannelCompEditor (Track& trackRef);
    ~ChannelCompEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void setMode (int modeIndex);
    void refreshModeButtons();

    Track& track;

    juce::Label titleLabel;
    juce::TextButton onButton  { "ON"   };
    juce::TextButton modeOpto  { "Opto" };
    juce::TextButton modeFet   { "FET"  };
    juce::TextButton modeVca   { "VCA"  };

    juce::Label  threshLabel,  ratioLabel,  attackLabel,  releaseLabel,  makeupLabel;
    juce::Slider threshKnob, ratioKnob, attackKnob, releaseKnob, makeupKnob;

    juce::Rectangle<int> grMeterArea;
    float displayedGrDb = 0.0f;
};
} // namespace adhdaw
