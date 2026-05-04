#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "CompMeterStrip.h"
#include "../session/Session.h"

namespace adhdaw
{
class ChannelStripComponent final : public juce::Component, private juce::Timer
{
public:
    ChannelStripComponent (int trackIndex, Track& trackRef, Session& sessionRef);
    ~ChannelStripComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

private:
    void timerCallback() override;

    int trackIndex;
    Track& track;
    Session& session;
    std::array<juce::uint32, ChannelStripParams::kNumBuses> lastBusColours {};  // for change detection in timerCallback
    float displayedGrDb = 0.0f;     // smoothed GR for the comp meter
    float displayedInputDb = -100.0f; // smoothed input level for the level meter
    float inputPeakHoldDb = -100.0f;  // brief peak hold marker
    int   inputPeakHoldFrames = 0;

    juce::Label nameLabel;

    juce::Rectangle<int> eqArea, compArea;

    juce::Slider hpfKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label  hpfLabel;

    juce::Label  eqHeaderLabel;
    juce::TextButton eqTypeButton { "B" };
    struct BandRow
    {
        std::unique_ptr<juce::Slider> gain;
        std::unique_ptr<juce::Slider> freq;
        std::unique_ptr<juce::Slider> q;     // populated for bell-only mid bands (HM/LM); nullptr for shelf bands (HF/LF)
        juce::Label labelLeft, labelRight;
        juce::Label rowLabel;
        juce::Label qLabel;   // "Q" header above the Q knob (bell bands only)
    };
    std::array<BandRow, 4> eqRows;

    // COMP section. UniversalCompressor exposes a different control set per
    // mode (Opto/FET/VCA model real hardware), so the strip holds the union
    // of all three and shows only the ones for the current mode.
    juce::TextButton compOnButton { "COMP" };  // header doubles as on/off toggle
    juce::TextButton compModeOpto { "OPT" };
    juce::TextButton compModeFet  { "FET" };
    juce::TextButton compModeVca  { "VCA" };

    // Opto (LA-2A): peak-reduction knob + gain knob + LIMIT toggle.
    juce::Slider     optoPeakRedKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     optoGainKnob    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label      optoPeakRedLabel, optoGainLabel;
    juce::TextButton optoLimitButton { "LIMIT" };

    // FET (1176): input + output + attack + release + ratio knob (5-step).
    juce::Slider     fetInputKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetOutputKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetAttackKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetReleaseKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetRatioKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label      fetInputLabel, fetOutputLabel, fetAttackLabel, fetReleaseLabel, fetRatioLabel;

    // VCA (classic): threshold via meter-strip drag handle (NOT a knob);
    // ratio + attack + release + output as knobs.
    juce::Slider     vcaRatioKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     vcaAttackKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     vcaReleaseKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     vcaOutputKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label      vcaRatioLabel, vcaAttackLabel, vcaReleaseLabel, vcaOutputLabel;

    std::unique_ptr<CompMeterStrip> compMeter;

    std::array<std::unique_ptr<juce::TextButton>, ChannelStripParams::kNumBuses> busButtons;

    juce::Slider panKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label  panLabel;
    juce::Slider faderSlider { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::Rectangle<int> inputMeterArea;
    juce::Rectangle<int> meterScaleArea;
    juce::Label inputPeakLabel;
    juce::Label threshMeterLabel;  // "THR" header above CompMeterStrip's drag handle
    juce::TextButton muteButton    { "M" };
    juce::TextButton soloButton    { "S" };
    juce::TextButton phaseButton   { juce::CharPointer_UTF8 ("\xc3\x98") };  // Ø — phase invert
    juce::TextButton armButton     { "ARM" };
    juce::TextButton monitorButton { "IN"  };
    juce::TextButton printButton   { "PRINT" };  // print EQ/comp to recording
    juce::ComboBox   inputSelector;

    void onHpfKnobChanged();
    void onInputSelectorChanged();
    void showColourMenu();
    void applyTrackColour (juce::Colour c);
    void refreshEqTypeButton();
    void setCompMode (int modeIndex);
    void refreshCompModeButtons();
};
} // namespace adhdaw
