#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../session/Session.h"

namespace adhdaw
{
class MasterStripComponent final : public juce::Component, private juce::Timer
{
public:
    explicit MasterStripComponent (MasterBusParams& paramsRef);
    ~MasterStripComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    MasterBusParams& params;

    juce::Label nameLabel;

    // Pultec-style Tube EQ.
    juce::TextButton eqButton    { "EQ" };
    juce::Slider     eqLfBoost   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfBoost   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqHfAtten   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqTubeDrive { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     eqOutputGain { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      eqLfLabel, eqHfBoostLabel, eqHfAttenLabel, eqDriveLabel, eqOutLabel;

    // Bus compressor (UniversalCompressor in Bus mode).
    juce::TextButton compButton    { "COMP" };
    juce::Slider     compThreshold { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compRatio     { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compAttack    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compRelease   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Slider     compMakeup    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label      compThrLabel, compRatLabel, compAtkLabel, compRelLabel, compMakLabel;

    juce::TextButton tapeButton { "TAPE" };
    juce::TextButton hqButton   { "HQ" };

    juce::Slider faderSlider { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };

    // Output stereo meter (post-master peak in dB, L/R split) + GR readout.
    // The meter sits to the RIGHT of the fader to match the channel-strip
    // layout. Two columns (L | R) live inside meterArea; we cache smoothed
    // and peak-hold values per channel.
    juce::Rectangle<int> meterArea;
    // Slim vertical bar showing the master bus comp's gain reduction.
    // Sits between the fader and the L/R output bars so the user can see
    // the compressor's contribution to the final signal at a glance.
    juce::Rectangle<int> grMeterArea;
    juce::Rectangle<int> faderScaleArea;
    juce::Label outputPeakLabel;
    juce::Label grPeakLabel;
    float displayedOutputLDb = -100.0f;
    float displayedOutputRDb = -100.0f;
    float displayedGrDb      = 0.0f;
    float outputPeakHoldLDb  = -100.0f;
    float outputPeakHoldRDb  = -100.0f;
    int   outputPeakHoldFramesL = 0;
    int   outputPeakHoldFramesR = 0;
};
} // namespace adhdaw
